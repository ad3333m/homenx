// HomeNX foreground video player.
// The FFmpeg decode/output split is inspired by dl3g0's Switube audio engine.
#include <switch.h>
#include <SDL2/SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

constexpr char kVideoDirectory[] = "/videos/";
constexpr int kSampleRate = 48000;
constexpr int kAudioFrameBytes = 4;

std::vector<std::string> videos() {
    std::vector<std::string> files;
    DIR* directory = opendir(kVideoDirectory);
    if (!directory) return files;
    while (files.size() < 512) {
        const auto* entry = readdir(directory);
        if (!entry) break;
        std::string name = entry->d_name;
        const auto dot = name.find_last_of('.');
        if (dot == std::string::npos) continue;
        std::string extension = name.substr(dot);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return std::tolower(ch); });
        if (extension != ".mp4" && extension != ".mkv" && extension != ".webm") continue;
        struct stat info{};
        if (stat((kVideoDirectory + name).c_str(), &info) == 0 && S_ISREG(info.st_mode))
            files.push_back(std::move(name));
    }
    closedir(directory);
    std::sort(files.begin(), files.end());
    return files;
}

std::string chooseVideo(PadState& input) {
    const auto files = videos();
    std::size_t selection = 0;
    bool redraw = true;
    consoleInit(nullptr);
    while (appletMainLoop()) {
        padUpdate(&input);
        const auto buttons = padGetButtonsDown(&input);
        if (buttons & HidNpadButton_B) break;
        if (!files.empty()) {
            if (buttons & HidNpadButton_A) {
                consoleExit(nullptr);
                return kVideoDirectory + files[selection];
            }
            if (buttons & HidNpadButton_Up) {
                selection = (selection + files.size() - 1) % files.size();
                redraw = true;
            }
            if (buttons & HidNpadButton_Down) {
                selection = (selection + 1) % files.size();
                redraw = true;
            }
        }
        if (redraw) {
            consoleClear();
            std::puts("HomeNX Video\n");
            if (files.empty()) std::puts("Copy an MP4, MKV or WebM file to /videos on the SD card.");
            const auto first = selection / 16 * 16;
            for (auto i = first; i < std::min(first + 16, files.size()); ++i)
                std::printf("%c %.60s\n", i == selection ? '>' : ' ', files[i].c_str());
            std::puts("\nA Play    B Exit");
            redraw = false;
        }
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
    return {};
}

struct Player {
    ~Player() {
        if (sound) SDL_CloseAudioDevice(sound);
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        if (sdlReady) SDL_Quit();
        swr_free(&resampler);
        sws_freeContext(scaler);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&video);
        avcodec_free_context(&audio);
        avformat_close_input(&source);
    }

    bool open(const char* path) {
        AVDictionary* options = nullptr;
        av_dict_set(&options, "protocol_whitelist", "file", 0);
        av_dict_set(&options, "format_whitelist", "mov,matroska,webm", 0);
        const int opened = avformat_open_input(&source, path, nullptr, &options);
        av_dict_free(&options);
        if (opened < 0 || avformat_find_stream_info(source, nullptr) < 0) return false;
        videoStream = openDecoder(AVMEDIA_TYPE_VIDEO, video);
        if (videoStream < 0 || video->width <= 0 || video->height <= 0 ||
            video->width > 1920 || video->height > 1080) return false;
        audioStream = openDecoder(AVMEDIA_TYPE_AUDIO, audio);
        if (SDL_Init(SDL_INIT_VIDEO | (audioStream >= 0 ? SDL_INIT_AUDIO : 0)) != 0) return false;
        sdlReady = true;
        window = SDL_CreateWindow("HomeNX Video", 0, 0, 1280, 720, SDL_WINDOW_FULLSCREEN);
        if (!window) return false;
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) return false;
        SDL_RenderSetLogicalSize(renderer, 1280, 720);
        SDL_Rect bounds{0, 0, 1280, 720};
        const AVRational aspect = av_guess_sample_aspect_ratio(source, source->streams[videoStream], nullptr);
        const double ratio = static_cast<double>(video->width) / video->height *
            (aspect.num > 0 && aspect.den > 0 ? av_q2d(aspect) : 1.0);
        if (ratio >= 1280.0 / 720) bounds.h = std::max(1, static_cast<int>(1280 / ratio));
        else bounds.w = std::max(1, static_cast<int>(720 * ratio));
        destination = {(1280 - bounds.w) / 2, (720 - bounds.h) / 2, bounds.w, bounds.h};
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                    bounds.w, bounds.h);
        if (!texture) return false;

        if (audioStream >= 0) {
            SDL_AudioSpec settings{};
            settings.freq = kSampleRate;
            settings.format = AUDIO_S16SYS;
            settings.channels = 2;
            settings.samples = 1024;
            sound = SDL_OpenAudioDevice(nullptr, 0, &settings, nullptr, 0);
            if (!sound) return false;
            SDL_PauseAudioDevice(sound, 0);
        }
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        return packet && frame;
    }

    bool play(PadState& pad) {
        startedAt = SDL_GetTicks64();
        while (controls(pad)) {
            const int next = av_read_frame(source, packet);
            if (next == AVERROR_EOF) {
                if (!decode(video, videoStream, nullptr, pad) ||
                    (audioStream >= 0 && !decode(audio, audioStream, nullptr, pad)))
                    return cancelled;
                while (sound && SDL_GetQueuedAudioSize(sound) > 0 && controls(pad))
                    svcSleepThread(10'000'000);
                return true;
            }
            if (next < 0) return false;
            const int stream = packet->stream_index;
            AVCodecContext* decoder = stream == videoStream ? video : stream == audioStream ? audio : nullptr;
            const bool ok = !decoder || decode(decoder, stream, packet, pad);
            av_packet_unref(packet);
            if (!ok) return cancelled;
        }
        return true;
    }

private:
    int openDecoder(AVMediaType type, AVCodecContext*& decoder) {
        const AVCodec* codec = nullptr;
        const int index = av_find_best_stream(source, type, -1, -1, &codec, 0);
        if (index < 0) return index;
        decoder = avcodec_alloc_context3(codec);
        if (!decoder || avcodec_parameters_to_context(decoder, source->streams[index]->codecpar) < 0)
            return -1;
        decoder->thread_count = type == AVMEDIA_TYPE_VIDEO ? 2 : 1;
        return avcodec_open2(decoder, codec, nullptr) == 0 ? index : -1;
    }

    bool controls(PadState& pad) {
        bool paused = false;
        auto pausedAt = SDL_GetTicks64();
        do {
            if (!appletMainLoop()) break;
            SDL_PumpEvents();
            padUpdate(&pad);
            const auto pressed = padGetButtonsDown(&pad);
            if (pressed & HidNpadButton_B) break;
            if (pressed & HidNpadButton_A) {
                paused = !paused;
                if (paused) pausedAt = SDL_GetTicks64();
                else startedAt += SDL_GetTicks64() - pausedAt;
                if (sound) SDL_PauseAudioDevice(sound, paused);
            }
            if (!paused) return true;
            svcSleepThread(16'000'000);
        } while (paused);
        cancelled = true;
        return false;
    }

    bool decode(AVCodecContext* decoder, int stream, const AVPacket* data, PadState& pad) {
        if (avcodec_send_packet(decoder, data) < 0) return false;
        for (;;) {
            const int rc = avcodec_receive_frame(decoder, frame);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) return false;
            const bool ok = stream == videoStream ? render(pad) : queueAudio(pad);
            av_frame_unref(frame);
            if (!ok) return false;
        }
    }

    bool render(PadState& pad) {
        if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
            const auto* stream = source->streams[videoStream];
            const auto pts = av_rescale_q(frame->best_effort_timestamp, stream->time_base, {1, 1000});
            // Anchor video to its first frame so edit lists or nonzero start times don't stall.
            if (!haveVideoPts) { firstVideoPts = pts; haveVideoPts = true; }
            const auto due = std::max<int64_t>(0, pts - firstVideoPts);
            if (due > static_cast<int64_t>(SDL_GetTicks64() - startedAt) + 5000) return false;
            while (static_cast<int64_t>(SDL_GetTicks64() - startedAt) < due) {
                if (!controls(pad)) return false;
                svcSleepThread(2'000'000);
            }
        }
        scaler = sws_getCachedContext(scaler, frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format), destination.w, destination.h,
            AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler) return false;
        void* pixels = nullptr;
        int stride = 0;
        if (SDL_LockTexture(texture, nullptr, &pixels, &stride) != 0) return false;
        uint8_t* output[] = {static_cast<uint8_t*>(pixels), nullptr, nullptr, nullptr};
        const int strides[] = {stride, 0, 0, 0};
        const int rows = sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, output, strides);
        SDL_UnlockTexture(texture);
        if (rows <= 0) return false;
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, &destination);
        SDL_RenderPresent(renderer);
        return controls(pad);
    }

    bool queueAudio(PadState& pad) {
        if (!resampler) {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, kSampleRate,
                &frame->ch_layout, static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr) < 0 ||
                swr_init(resampler) < 0) return false;
        }
        const int capacity = swr_get_out_samples(resampler, frame->nb_samples);
        if (capacity <= 0 || capacity > kSampleRate) return false;
        samples.resize(static_cast<std::size_t>(capacity) * kAudioFrameBytes);
        uint8_t* output = samples.data();
        const int frames = swr_convert(resampler, &output, capacity,
            const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
        if (frames < 0) return false;
        while (SDL_GetQueuedAudioSize(sound) > kSampleRate * kAudioFrameBytes / 3) {
            if (!controls(pad)) return false;
            svcSleepThread(2'000'000);
        }
        return SDL_QueueAudio(sound, samples.data(), frames * kAudioFrameBytes) == 0;
    }

    AVFormatContext* source = nullptr;
    AVCodecContext* video = nullptr;
    AVCodecContext* audio = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* scaler = nullptr;
    SwrContext* resampler = nullptr;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_AudioDeviceID sound = 0;
    SDL_Rect destination{};
    std::vector<uint8_t> samples;
    uint64_t startedAt = 0;
    int64_t firstVideoPts = 0;
    int videoStream = -1, audioStream = -1;
    bool haveVideoPts = false, sdlReady = false, cancelled = false;
};

} // namespace

int main() {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad{};
    padInitializeDefault(&pad);
    const auto path = chooseVideo(pad);
    if (path.empty()) return 0;
    {
        Player player;
        if (player.open(path.c_str()) && player.play(pad)) return 0;
    }
    consoleInit(nullptr);
    std::puts("HomeNX couldn't play this file. Try a video at 1080p or below.\nB Exit");
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_B) break;
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
}
