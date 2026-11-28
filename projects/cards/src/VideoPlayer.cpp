#include "VideoPlayer.hpp"

#include <switch.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef CARDS_VIDEO
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace cards {
namespace {
constexpr int kOutW = 960, kOutH = 540; // (half the work of 1280x720; the screen scales it up)
std::uint64_t nowNs() { return armTicksToNs(armGetSystemTick()); }
void nap(int ms) { svcSleepThread(static_cast<std::int64_t>(ms) * 1'000'000LL); }
} // namespace

VideoPlayer::~VideoPlayer() { close(); }

void VideoPlayer::close() {
    m_stop.store(true);
    if (m_thread.joinable()) m_thread.join();
#ifdef CARDS_VIDEO
    if (m_hooked) {
        Mix_HookMusic(nullptr, nullptr); // HOME's music again, where it was
        m_hooked = false;
    }
#endif
}

bool VideoPlayer::takeFrame(std::vector<std::uint8_t>& rgba, int& w, int& h) {
    std::lock_guard<std::mutex> lk(m_frameMu);
    if (!m_fresh) return false;
    rgba = m_frame;
    w = m_w;
    h = m_h;
    m_fresh = false;
    return true;
}

void VideoPlayer::skip(float seconds) {
    m_seekBy.store(m_seekBy.load() + seconds);
    m_seekWanted.store(true);
}

void VideoPlayer::mixHook(void* self, std::uint8_t* stream, int len) {
    auto* p = static_cast<VideoPlayer*>(self);
    std::memset(stream, 0, static_cast<size_t>(len));
    if (p->m_paused.load()) return;
    std::lock_guard<std::mutex> lk(p->m_soundMu);
    const size_t want = static_cast<size_t>(len) / sizeof(std::int16_t);
    const size_t have = p->m_sound.size() - p->m_soundAt;
    const size_t n = std::min(want, have);
    std::memcpy(stream, p->m_sound.data() + p->m_soundAt, n * sizeof(std::int16_t));
    p->m_soundAt += n;
    if (p->m_soundAt > 1 << 16) { // (keep the buffer from growing)
        p->m_sound.erase(p->m_sound.begin(), p->m_sound.begin() + static_cast<std::ptrdiff_t>(p->m_soundAt));
        p->m_soundAt = 0;
    }
    p->m_heard.fetch_add(n / 2);
}

double VideoPlayer::clock() const {
    if (m_rate > 0) return m_base.load() + static_cast<double>(m_heard.load()) / m_rate;
    return m_base.load() + static_cast<double>(nowNs() - m_wallStart.load()) / 1e9;
}

bool VideoPlayer::open(const std::string& path) {
#ifdef CARDS_VIDEO
    close();
    m_stop.store(false);
    m_paused.store(false);
    m_ended.store(false);
    m_pos.store(0.f);
    m_dur.store(0.f);
    m_fresh = false;
    m_thread = std::thread([this, path]() { run(path); });
    return true;
#else
    (void)path;
    return false;
#endif
}

void VideoPlayer::run(std::string path) {
#ifdef CARDS_VIDEO
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        m_ended.store(true);
        return;
    }
    avformat_find_stream_info(fmt, nullptr);
    if (fmt->duration > 0) m_dur.store(static_cast<float>(fmt->duration) / AV_TIME_BASE);
    const int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vi, nullptr, 0);
    auto openCodec = [&](int index) -> AVCodecContext* {
        if (index < 0) return nullptr;
        const AVCodec* codec = avcodec_find_decoder(fmt->streams[index]->codecpar->codec_id);
        if (!codec) return nullptr;
        AVCodecContext* c = avcodec_alloc_context3(codec);
        if (!c) return nullptr;
        avcodec_parameters_to_context(c, fmt->streams[index]->codecpar);
        c->thread_count = 3;
        if (avcodec_open2(c, codec, nullptr) < 0) avcodec_free_context(&c);
        return c;
    };
    AVCodecContext* vc = openCodec(vi);
    AVCodecContext* ac = openCodec(ai);
    if (!vc) {
        avcodec_free_context(&ac);
        avformat_close_input(&fmt);
        m_ended.store(true);
        return;
    }

    // The sound: converted to what the mixer plays, and handed to it in place of HOME's music.
    SwrContext* swr = nullptr;
    int freq = 0, channels = 0;
    Uint16 format = 0;
    if (ac && Mix_QuerySpec(&freq, &format, &channels) && channels == 2) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, freq, &ac->ch_layout, ac->sample_fmt, ac->sample_rate, 0, nullptr) >= 0
            && swr_init(swr) >= 0) {
            m_rate = freq;
            Mix_HookMusic(&VideoPlayer::mixHook, this);
            m_hooked = true;
        } else {
            swr_free(&swr);
        }
    }
    m_base.store(0.0);
    m_heard.store(0);
    m_wallStart.store(nowNs());

    SwsContext* sws = nullptr;
    std::vector<std::uint8_t> rgba(static_cast<size_t>(kOutW) * kOutH * 4);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const AVRational vtb = fmt->streams[vi]->time_base;
    std::uint64_t pausedAt = 0;
    bool eof = false;

    auto showFrame = [&](AVFrame* f) {
        const double pts = f->best_effort_timestamp == AV_NOPTS_VALUE ? clock() : f->best_effort_timestamp * av_q2d(vtb);
        while (!m_stop.load() && !m_seekWanted.load() && (m_paused.load() || clock() + 0.005 < pts)) nap(4); // its time
        if (m_stop.load() || m_seekWanted.load()) return;
        if (clock() > pts + 0.12) return; // late: dropped, to catch up
        sws = sws_getCachedContext(sws, f->width, f->height, static_cast<AVPixelFormat>(f->format), kOutW, kOutH, AV_PIX_FMT_RGBA,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) return;
        std::uint8_t* dst[1] = {rgba.data()};
        const int lines[1] = {kOutW * 4};
        sws_scale(sws, f->data, f->linesize, 0, f->height, dst, lines);
        {
            std::lock_guard<std::mutex> lk(m_frameMu);
            m_frame = rgba;
            m_w = kOutW;
            m_h = kOutH;
            m_fresh = true;
        }
        m_pos.store(static_cast<float>(pts));
    };
    auto takeSound = [&](AVFrame* f) {
        if (!swr) return;
        const int cap = swr_get_out_samples(swr, f->nb_samples);
        if (cap <= 0) return;
        std::vector<std::int16_t> out(static_cast<size_t>(cap) * 2);
        std::uint8_t* dst = reinterpret_cast<std::uint8_t*>(out.data());
        const int n = swr_convert(swr, &dst, cap, const_cast<const std::uint8_t**>(f->extended_data), f->nb_samples);
        if (n <= 0) return;
        // Not more than a second ahead of what's heard (the mixer catches up).
        while (!m_stop.load() && !m_seekWanted.load()) {
            size_t queued;
            {
                std::lock_guard<std::mutex> lk(m_soundMu);
                queued = (m_sound.size() - m_soundAt) / 2;
            }
            if (queued < static_cast<size_t>(m_rate)) break;
            nap(10);
        }
        std::lock_guard<std::mutex> lk(m_soundMu);
        m_sound.insert(m_sound.end(), out.begin(), out.begin() + n * 2);
    };
    auto drain = [&](AVCodecContext* c, bool video) {
        while (!m_stop.load() && avcodec_receive_frame(c, frame) == 0) {
            if (video) showFrame(frame);
            else takeSound(frame);
            av_frame_unref(frame);
            if (m_seekWanted.load()) break;
        }
    };

    while (!m_stop.load()) {
        if (m_seekWanted.exchange(false)) { // skipping: from the new place, the sound with it
            const float by = m_seekBy.exchange(0.f);
            const double target = std::clamp(clock() + by, 0.0, std::max(0.0, static_cast<double>(m_dur.load()) - 0.5));
            av_seek_frame(fmt, -1, static_cast<std::int64_t>(target * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(vc);
            if (ac) avcodec_flush_buffers(ac);
            {
                std::lock_guard<std::mutex> lk(m_soundMu);
                m_sound.clear();
                m_soundAt = 0;
            }
            m_base.store(target);
            m_heard.store(0);
            m_wallStart.store(nowNs());
            eof = false;
            continue;
        }
        if (m_paused.load()) { // (the wall clock doesn't count a pause)
            if (!pausedAt) pausedAt = nowNs();
            nap(20);
            continue;
        }
        if (pausedAt) {
            m_wallStart.store(m_wallStart.load() + (nowNs() - pausedAt));
            pausedAt = 0;
        }
        if (eof) { // the end: once the last of the sound has been heard
            size_t queued;
            {
                std::lock_guard<std::mutex> lk(m_soundMu);
                queued = m_sound.size() - m_soundAt;
            }
            if (queued == 0 || m_rate == 0) {
                m_ended.store(true);
                break;
            }
            nap(20);
            continue;
        }
        if (av_read_frame(fmt, pkt) < 0) {
            avcodec_send_packet(vc, nullptr);
            drain(vc, true);
            if (ac) {
                avcodec_send_packet(ac, nullptr);
                drain(ac, false);
            }
            eof = true;
            continue;
        }
        if (pkt->stream_index == vi) {
            if (avcodec_send_packet(vc, pkt) >= 0) drain(vc, true);
        } else if (ac && pkt->stream_index == ai) {
            if (avcodec_send_packet(ac, pkt) >= 0) drain(ac, false);
        }
        av_packet_unref(pkt);
    }

    if (m_hooked) { // (the mixer stops asking before the sound is let go)
        Mix_HookMusic(nullptr, nullptr);
        m_hooked = false;
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    sws_freeContext(sws);
    swr_free(&swr);
    avcodec_free_context(&vc);
    avcodec_free_context(&ac);
    avformat_close_input(&fmt);
#else
    (void)path;
    m_ended.store(true);
#endif
}

} // namespace cards
