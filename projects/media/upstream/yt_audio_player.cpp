#include "yt_audio_player.hpp"

#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/intreadwrite.h>
#include <opus/opus.h>
#include <nestegg/nestegg.h>
}

#include "NXAudioSink.h"
#include "NXConsoleLog.h"
#include "thread_helper.hpp"

namespace switube {

namespace {

#ifdef SWITUBE_BACKGROUND_SERVICE
constexpr size_t AVIO_BUFFER_SIZE = 8 * 1024;
#else
constexpr size_t AVIO_BUFFER_SIZE = 32 * 1024;
#endif
constexpr size_t CHUNK_SIZE = 512 * 1024; // 512 KiB per HTTP range

struct BufferReader {
  std::vector<uint8_t> buffer;
  size_t readPos = 0;
  FILE* localFp = nullptr;
  long localFileSize = 0;
  std::atomic<bool> isDownloading{true};
  std::atomic<bool> downloadFailed{false};
  std::shared_ptr<std::atomic<bool>> stopToken;
  std::mutex mutex;

  ~BufferReader() {
    if (localFp) {
      fclose(localFp);
      localFp = nullptr;
    }
  }
};


int AvioRead(void* opaque, uint8_t* buf, int buf_size) {
  auto* reader = static_cast<BufferReader*>(opaque);
  while (!reader->stopToken->load()) {
    {
      std::lock_guard<std::mutex> lock(reader->mutex);
      if (reader->localFp) {
        size_t n = fread(buf, 1, static_cast<size_t>(buf_size), reader->localFp);
        if (n > 0) {
          reader->readPos += n;
          return static_cast<int>(n);
        }
        return AVERROR_EOF;
      }
      if (reader->readPos < reader->buffer.size()) {
        int avail = static_cast<int>(reader->buffer.size() - reader->readPos);
        int toRead = std::min(buf_size, avail);
        std::memcpy(buf, reader->buffer.data() + reader->readPos, toRead);
        reader->readPos += toRead;
        return toRead;
      }
    }
    if (!reader->isDownloading.load()) {
      return AVERROR_EOF;
    }
    if (reader->downloadFailed.load()) {
      return AVERROR(EIO);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return AVERROR_EXIT;
}

int64_t AvioSeek(void* opaque, int64_t offset, int whence) {
  auto* reader = static_cast<BufferReader*>(opaque);
  std::lock_guard<std::mutex> lock(reader->mutex);

  // IMPORTANTE: Para streams WebM/Opus de YouTube, NUNCA devolver el tamaño estático en AVSEEK_SIZE.
  // Si FFmpeg recibe el tamaño total, asume que es un archivo estático indexado e intenta
  // hacer seek al final del buffer buscando Cues/Index, alcanzando EOF prematuramente y
  // deteniendo la reproducción a los pocos segundos. Devolver -1 garantiza reproducción secuencial fluida.
  if (whence & AVSEEK_SIZE) {
    return -1;
  }

  int baseWhence = whence & ~AVSEEK_FORCE;

  if (reader->localFp) {
    if (baseWhence != SEEK_SET && baseWhence != SEEK_CUR && baseWhence != SEEK_END) {
      return -1;
    }
    if (fseek(reader->localFp, static_cast<long>(offset), baseWhence) != 0) {
      return -1;
    }
    long pos = ftell(reader->localFp);
    if (pos < 0) return -1;
    reader->readPos = static_cast<size_t>(pos);
    return pos;
  }

  int64_t newPos = 0;
  if (baseWhence == SEEK_SET) {
    newPos = offset;
  } else if (baseWhence == SEEK_CUR) {
    newPos = static_cast<int64_t>(reader->readPos) + offset;
  } else if (baseWhence == SEEK_END) {
    newPos = static_cast<int64_t>(reader->buffer.size()) + offset;
  } else {
    return -1;
  }

  if (newPos < 0) return -1;
  if (!reader->isDownloading.load() && static_cast<size_t>(newPos) > reader->buffer.size()) {
    newPos = static_cast<int64_t>(reader->buffer.size());
  }
  reader->readPos = static_cast<size_t>(newPos);
  return reader->readPos;
}

nestegg_io webmIo(FILE* file) {
  return {
    [](void* out, size_t bytes, void* user) -> int64_t {
      auto* fp = static_cast<FILE*>(user);
      const auto count = fread(out, 1, bytes, fp);
      return ferror(fp) ? -1 : static_cast<int64_t>(count);
    },
    [](int64_t offset, int whence, void* user) {
      return fseeko(static_cast<FILE*>(user), offset, whence);
    },
    [](void* user) -> int64_t { return ftello(static_cast<FILE*>(user)); },
    file
  };
}

} // namespace

YtAudioPlayer& YtAudioPlayer::getInstance() {
  static YtAudioPlayer instance;
  return instance;
}

YtAudioPlayer::YtAudioPlayer() {
  sink_ = std::make_unique<NXAudioSink>();
  sink_->setParams(48000, 2, 16);
  sink_->setVolume(volume_.load());
}

YtAudioPlayer::~YtAudioPlayer() {
  stop();
}

bool YtAudioPlayer::init() {
  return sink_ && sink_->ok();
}

void YtAudioPlayer::shutdown() {
  stop();
}

void YtAudioPlayer::play(const std::string& streamUrl, uint32_t durationMs, uint64_t contentLength) {
  uint64_t gen = 0;
  std::shared_ptr<std::atomic<bool>> newToken;

  {
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (currentToken_) {
      currentToken_->store(true);
    }

    newToken = std::make_shared<std::atomic<bool>>(false);
    currentToken_ = newToken;

    isPaused_ = false;
    isPlaying_ = true;
    currentPositionMs_ = 0;
    durationMs_ = durationMs;
    gen = ++currentGeneration_;
  }

  // flush() only sets an atomic flag; avoid blocking the UI thread on sinkMutex_
  if (sink_) {
    sink_->flush();
  }

  nxlog::pushf("YtAudioPlayer: starting playback (gen %llu, bytes: %llu)...",
               static_cast<unsigned long long>(gen), static_cast<unsigned long long>(contentLength));

#ifdef SWITUBE_BACKGROUND_SERVICE
  constexpr size_t kDecoderStackSize = 128 * 1024;
#else
  constexpr size_t kDecoderStackSize = 1024 * 1024;
#endif
  launchWorkerWithStack([this, streamUrl, durationMs, contentLength, gen, newToken]() {
    audioWorker(streamUrl, durationMs, contentLength, gen, newToken);
  }, kDecoderStackSize);
}

void YtAudioPlayer::pause() {
  isPaused_ = true;
  isPlaying_ = false;
}

void YtAudioPlayer::resume() {
  isPaused_ = false;
  isPlaying_ = true;
}

void YtAudioPlayer::stop() {
  {
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (currentToken_) {
      currentToken_->store(true);
    }
    isPlaying_ = false;
    isPaused_ = false;
    currentPositionMs_ = 0;
  }
  // flush() only sets an atomic flag; avoid blocking the UI thread on sinkMutex_
  if (sink_) {
    sink_->flush();
  }
}

void YtAudioPlayer::seekTo(uint32_t positionMs) {
  currentPositionMs_ = positionMs;
  seekTargetMs_.store(static_cast<int64_t>(positionMs));
}

void YtAudioPlayer::setVolume(float volume) {
  volume_ = volume;
  // setVolume() stores to an atomic float; avoid blocking the UI thread on sinkMutex_
  if (sink_) {
    sink_->setVolume(volume);
  }
}

void YtAudioPlayer::playWebmOpus(nestegg* file, unsigned track,
                                 const std::shared_ptr<std::atomic<bool>>& stop) {
  unsigned char* header = nullptr;
  size_t length = 0;
  if (nestegg_track_codec_data(file, track, 0, &header, &length) < 0 ||
      length < 19 || memcmp(header, "OpusHead", 8) || header[9] < 1 ||
      header[9] > 2 || header[18] != 0) return;
  std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder(
      opus_decoder_create(48000, 2, nullptr), opus_decoder_destroy);
  if (!decoder) return;
  opus_decoder_ctl(decoder.get(), OPUS_SET_GAIN(static_cast<int16_t>(AV_RL16(header + 16))));
  int skip = AV_RL16(header + 10);
  uint64_t duration = 0;
  if (!durationMs_.load() && nestegg_duration(file, &duration) == 0)
    durationMs_ = duration / 1000000;
  std::vector<int16_t> pcm(5760 * 2);
  bool started = false;
  while (!stop->load()) {
    const auto seek = seekTargetMs_.exchange(-1);
    if (seek >= 0 && nestegg_track_seek(file, track, seek * 1000000) == 0) {
      opus_decoder_ctl(decoder.get(), OPUS_RESET_STATE);
      skip = seek == 0 ? AV_RL16(header + 10) : 0;
      sink_->flush();
    }
    if (isPaused_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      continue;
    }
    nestegg_packet* raw = nullptr;
    const int result = nestegg_read_packet(file, &raw);
    if (result <= 0) {
      std::function<void()> callback;
      {
        std::lock_guard lock(controlMutex_);
        if (!stop->load() && result == 0) callback = onComplete_;
      }
      if (callback) callback();
      return;
    }
    std::unique_ptr<nestegg_packet, decltype(&nestegg_free_packet)> packet(raw, nestegg_free_packet);
    unsigned index, count;
    uint64_t time;
    if (nestegg_packet_track(raw, &index) < 0 || index != track) continue;
    if (nestegg_packet_count(raw, &count) < 0 || nestegg_packet_tstamp(raw, &time) < 0) return;
    for (unsigned i = 0; i < count && !stop->load(); ++i) {
      unsigned char* data;
      size_t bytes;
      if (nestegg_packet_data(raw, i, &data, &bytes) < 0 || !bytes || bytes > INT32_MAX) return;
      int frames = opus_decode(decoder.get(), data, bytes, pcm.data(), 5760, 0);
      if (frames < 0) return;
      const int leading = std::min(skip, frames);
      skip -= leading;
      frames -= leading;
      int64_t padding = 0;
      if (i + 1 == count && nestegg_packet_discard_padding(raw, &padding) == 0 && padding > 0)
        frames -= std::min<int64_t>(frames, (std::min<int64_t>(padding, 120000000) * 48000 + 500000000) / 1000000000);
      if (!frames) continue;
      if (!started) {
        started = true;
        std::function<void()> callback;
        {
          std::lock_guard lock(controlMutex_);
          if (!stop->load()) callback = onStart_;
        }
        if (callback) callback();
      }
      if (stop->load()) return;
      sink_->feedPCMFrames(reinterpret_cast<const uint8_t*>(pcm.data() + leading * 2),
                           frames * 2 * sizeof(int16_t));
      currentPositionMs_ = time / 1000000;
      time += static_cast<uint64_t>(frames) * 1000000000 / 48000;
    }
  }
}

void YtAudioPlayer::audioWorker(std::string url, uint32_t durMs, uint64_t contentLength, uint64_t generation,
                                std::shared_ptr<std::atomic<bool>> stopToken) {
  std::lock_guard<std::mutex> workerLock(workerMutex_);
  if (stopToken->load() || generation != currentGeneration_.load()) {
    return;
  }

  bool isLocal = (url.rfind("/", 0) == 0 || url.rfind("sdmc:/", 0) == 0);
  AVFormatContext* fmtCtx = nullptr;
  AVIOContext* avioCtx = nullptr;
  auto reader = std::make_shared<BufferReader>();
  reader->stopToken = stopToken;

  if (isLocal) {
    nxlog::pushf("YtAudioPlayer: playing local file '%s'", url.c_str());
    FILE* fp = fopen(url.c_str(), "rb");
    if (!fp && url.rfind("/", 0) == 0) {
      std::string sdmcUrl = "sdmc:" + url;
      fp = fopen(sdmcUrl.c_str(), "rb");
    }
    if (!fp) {
      nxlog::pushf("YtAudioPlayer error: cannot open local file: %s", url.c_str());
      return;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0) {
      fclose(fp);
      nxlog::pushf("YtAudioPlayer error: empty local file: %s", url.c_str());
      return;
    }
#ifdef SWITUBE_BACKGROUND_SERVICE
    reader->localFp = fp;
    reader->localFileSize = fsize;
    unsigned char magic[4];
    const bool webm = fread(magic, 1, sizeof(magic), fp) == sizeof(magic) &&
                      !memcmp(magic, "\x1a\x45\xdf\xa3", 4);
    rewind(fp);
    if (webm) {
      nestegg* raw = nullptr;
      if (nestegg_init(&raw, webmIo(fp), nullptr, fsize) < 0) return;
      std::unique_ptr<nestegg, decltype(&nestegg_destroy)> container(raw, nestegg_destroy);
      unsigned tracks = 0;
      if (nestegg_track_count(raw, &tracks) < 0) return;
      for (unsigned track = 0; track < tracks; ++track) {
        if (nestegg_track_codec_id(raw, track) == NESTEGG_CODEC_OPUS) {
          playWebmOpus(raw, track, stopToken);
          return;
        }
      }
      container.reset();
      rewind(fp);
    }
    reader->isDownloading = false;
#else
    reader->buffer.resize(static_cast<size_t>(fsize));
    size_t readBytes = fread(reader->buffer.data(), 1, static_cast<size_t>(fsize), fp);
    fclose(fp);
    if (readBytes == 0) {
      nxlog::pushf("YtAudioPlayer error: fread failed on local file: %s", url.c_str());
      return;
    }
    reader->buffer.resize(readBytes);
    reader->isDownloading = false;
#endif
  } else {
    return; // HomeNX Media reads files on the SD card only.
  }

  if (stopToken->load()) return;

  uint8_t* avioBuffer = static_cast<uint8_t*>(av_malloc(AVIO_BUFFER_SIZE));
  if (!avioBuffer) {
    nxlog::push("YtAudioPlayer error: av_malloc(AVIO_BUFFER_SIZE) failed");
    return;
  }
  avioCtx = avio_alloc_context(avioBuffer, AVIO_BUFFER_SIZE, 0, reader.get(), AvioRead, nullptr, AvioSeek);
  if (!avioCtx) {
    av_free(avioBuffer);
    nxlog::push("YtAudioPlayer error: avio_alloc_context failed");
    return;
  }

  fmtCtx = avformat_alloc_context();
  if (!fmtCtx) {
    if (avioCtx->buffer) av_freep(&avioCtx->buffer);
    avio_context_free(&avioCtx);
    nxlog::push("YtAudioPlayer error: avformat_alloc_context failed");
    return;
  }
  fmtCtx->pb = avioCtx;
  // Decode the audio packets without importing the container’s embedded seek index.
  fmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_IGNIDX;

  if (avformat_open_input(&fmtCtx, nullptr, nullptr, nullptr) < 0) {
    nxlog::push("YtAudioPlayer error: avformat_open_input failed");
    if (fmtCtx) {
      fmtCtx->pb = nullptr;
      avformat_close_input(&fmtCtx);
    }
    if (avioCtx) {
      if (avioCtx->buffer) av_freep(&avioCtx->buffer);
      avio_context_free(&avioCtx);
    }
    return;
  }

  if (stopToken->load() || generation != currentGeneration_.load()) {
    if (fmtCtx) {
      fmtCtx->pb = nullptr;
      avformat_close_input(&fmtCtx);
    }
    if (avioCtx) {
      if (avioCtx->buffer) av_freep(&avioCtx->buffer);
      avio_context_free(&avioCtx);
    }
    return;
  }

  int streamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  const bool isOpus = streamIdx >= 0 &&
      fmtCtx->streams[streamIdx]->codecpar->codec_id == AV_CODEC_ID_OPUS;
  if (!isOpus && avformat_find_stream_info(fmtCtx, nullptr) >= 0)
    streamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  const auto* params = streamIdx >= 0 ? fmtCtx->streams[streamIdx]->codecpar : nullptr;
  const auto* codec = params && !isOpus ? avcodec_find_decoder(params->codec_id) : nullptr;

  auto closeInput = [&] {
    fmtCtx->pb = nullptr;
    avformat_close_input(&fmtCtx);
    av_freep(&avioCtx->buffer);
    avio_context_free(&avioCtx);
  };
  std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> opus(nullptr, opus_decoder_destroy);
  int opusSkip = 0;
  if (isOpus) {
    // libopus handles mono and stereo packets and always returns stereo PCM.
    // YouTube's Opus downloads use channel mapping family zero.
    const auto* header = params->extradata;
    if (!header || params->extradata_size < 19 || std::memcmp(header, "OpusHead", 8) != 0 ||
        header[9] < 1 || header[9] > 2 || header[18] != 0) {
      nxlog::push("YtAudioPlayer: unsupported Opus channel mapping");
      closeInput();
      return;
    }
    opus.reset(opus_decoder_create(48000, 2, nullptr));
    if (!opus) {
      nxlog::push("YtAudioPlayer: libopus initialization failed");
      closeInput();
      return;
    }
    opusSkip = AV_RL16(header + 10);
    opus_decoder_ctl(opus.get(), OPUS_SET_GAIN(static_cast<int>(static_cast<int16_t>(AV_RL16(header + 16)))));
  } else if (!codec) {
    nxlog::push("YtAudioPlayer: no supported audio stream");
    closeInput();
    return;
  }

  AVCodecContext* codecCtx = opus ? nullptr : avcodec_alloc_context3(codec);
  if (!opus && !codecCtx) {
    nxlog::push("YtAudioPlayer error: avcodec_alloc_context3 failed (OOM)");
    if (fmtCtx) {
      fmtCtx->pb = nullptr;
      avformat_close_input(&fmtCtx);
    }
    if (avioCtx) {
      if (avioCtx->buffer) av_freep(&avioCtx->buffer);
      avio_context_free(&avioCtx);
    }
    return;
  }
  if (!opus && (avcodec_parameters_to_context(codecCtx, params) < 0 ||
      avcodec_open2(codecCtx, codec, nullptr) < 0)) {
    nxlog::push("YtAudioPlayer error: avcodec_open2 failed");
    avcodec_free_context(&codecCtx);
    if (fmtCtx) {
      fmtCtx->pb = nullptr;
      avformat_close_input(&fmtCtx);
    }
    if (avioCtx) {
      if (avioCtx->buffer) av_freep(&avioCtx->buffer);
      avio_context_free(&avioCtx);
    }
    return;
  }

  SwrContext* swrCtx = nullptr;
  if (!opus) {
    // Configurar SwrContext para remuestrear a 48000 Hz, S16 estéreo
    AVChannelLayout outChLayout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout inChLayout = {};
    if (codecCtx->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC && codecCtx->ch_layout.nb_channels > 0) {
      av_channel_layout_copy(&inChLayout, &codecCtx->ch_layout);
    } else {
      int channels = codecCtx->ch_layout.nb_channels > 0 ? codecCtx->ch_layout.nb_channels : 2;
      av_channel_layout_default(&inChLayout, channels);
    }
    int swrAllocRet = swr_alloc_set_opts2(&swrCtx,
                                          &outChLayout,
                                          AV_SAMPLE_FMT_S16,
                                          48000,
                                          &inChLayout,
                                          codecCtx->sample_fmt,
                                          codecCtx->sample_rate,
                                          0, nullptr);
    av_channel_layout_uninit(&inChLayout);
    if (swrAllocRet < 0 || !swrCtx || swr_init(swrCtx) < 0) {
      nxlog::push("YtAudioPlayer error: swr_init failed");
      if (swrCtx) swr_free(&swrCtx);
      avcodec_free_context(&codecCtx);
      if (fmtCtx) {
        fmtCtx->pb = nullptr;
        avformat_close_input(&fmtCtx);
      }
      if (avioCtx) {
        if (avioCtx->buffer) av_freep(&avioCtx->buffer);
        avio_context_free(&avioCtx);
      }
      return;
    }
  }

  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = opus ? nullptr : av_frame_alloc();
  if (!pkt || (!opus && !frame)) {
    nxlog::push("YtAudioPlayer error: av_packet_alloc/av_frame_alloc failed");
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    swr_free(&swrCtx);
    avcodec_free_context(&codecCtx);
    if (fmtCtx) {
      fmtCtx->pb = nullptr;
      avformat_close_input(&fmtCtx);
    }
    if (avioCtx) {
      if (avioCtx->buffer) av_freep(&avioCtx->buffer);
      avio_context_free(&avioCtx);
    }
    return;
  }
  AVRational tb = fmtCtx->streams[streamIdx]->time_base;

  constexpr int MAX_OUT_SAMPLES = 8192;
  std::vector<int16_t> pcmOut(MAX_OUT_SAMPLES * 2);

  nxlog::pushf("YtAudioPlayer: audio decoding started (%d Hz -> 48000 Hz, codec: %s)",
               opus ? 48000 : codecCtx->sample_rate, opus ? "libopus" : codec->name);

  bool completed = false;
  bool firstFrameNotified = false;

  auto feed = [&](const int16_t* pcm, int frames, int64_t pts) {
    if (frames <= 0 || stopToken->load()) return;
    if (!firstFrameNotified) {
      firstFrameNotified = true;
      std::function<void()> callback;
      {
        std::lock_guard lock(controlMutex_);
        callback = onStart_;
      }
      if (callback && !stopToken->load()) callback();
    }
    {
      std::lock_guard lock(sinkMutex_);
      if (!stopToken->load() && sink_)
        sink_->feedPCMFrames(reinterpret_cast<const uint8_t*>(pcm), frames * 2 * sizeof(int16_t));
    }
    if (pts != AV_NOPTS_VALUE)
      currentPositionMs_ = static_cast<uint32_t>(std::clamp(pts * av_q2d(tb) * 1000.0, 0.0,
                                                            static_cast<double>(UINT32_MAX)));
  };

  while (!stopToken->load() && generation == currentGeneration_.load()) {
    int64_t seekMs = seekTargetMs_.exchange(-1);
    if (seekMs >= 0) {
      int64_t targetPts = static_cast<int64_t>((static_cast<double>(seekMs) / 1000.0) / av_q2d(tb));
      int retSeek = av_seek_frame(fmtCtx, streamIdx, targetPts, AVSEEK_FLAG_BACKWARD);
      if (retSeek >= 0) {
        if (opus) {
          opus_decoder_ctl(opus.get(), OPUS_RESET_STATE);
          opusSkip = seekMs == 0 ? AV_RL16(params->extradata + 10) : 0;
        } else {
          avcodec_flush_buffers(codecCtx);
        }
        {
          std::lock_guard<std::mutex> lock(sinkMutex_);
          if (sink_) sink_->flush();
        }
        currentPositionMs_.store(static_cast<uint32_t>(seekMs));
        nxlog::pushf("YtAudioPlayer: seeked to %lld ms", seekMs);
      } else {
        nxlog::pushf("YtAudioPlayer: av_seek_frame failed (%d) for %lld ms", retSeek, seekMs);
      }
    }

    if (isPaused_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      continue;
    }

    int ret = av_read_frame(fmtCtx, pkt);
    if (ret < 0) {
      if (ret == AVERROR_EOF) {
        completed = true;
      }
      break;
    }

    if (pkt->stream_index == streamIdx) {
      if (opus) {
        const int decoded = opus_decode(opus.get(), pkt->data, pkt->size,
                                        pcmOut.data(), 5760, 0);
        if (decoded < 0) {
          nxlog::pushf("YtAudioPlayer: libopus decode failed (%d)", decoded);
          break;
        }
        const int skip = std::min(decoded, opusSkip);
        opusSkip -= skip;
        size_t trimSize = 0;
        const auto* trim = av_packet_get_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, &trimSize);
        const int endTrim = trimSize >= 8 ? std::min<uint32_t>(AV_RL32(trim + 4), decoded - skip) : 0;
        feed(pcmOut.data() + skip * 2, decoded - skip - endTrim, pkt->pts);
      } else if (avcodec_send_packet(codecCtx, pkt) == 0) {
        while (avcodec_receive_frame(codecCtx, frame) == 0 && !stopToken->load()) {
          auto* out = reinterpret_cast<uint8_t*>(pcmOut.data());
          const auto** in = const_cast<const uint8_t**>(frame->extended_data);
          const int decoded = swr_convert(swrCtx, &out, MAX_OUT_SAMPLES, in, frame->nb_samples);
          feed(pcmOut.data(), decoded, frame->pts);
        }
      }
    }
    av_packet_unref(pkt);
  }

  if (frame) av_frame_free(&frame);
  if (pkt) av_packet_free(&pkt);
  if (swrCtx) swr_free(&swrCtx);
  if (codecCtx) avcodec_free_context(&codecCtx);
  if (fmtCtx) {
    fmtCtx->pb = nullptr;
    avformat_close_input(&fmtCtx);
  }
  if (avioCtx) {
    if (avioCtx->buffer) av_freep(&avioCtx->buffer);
    avio_context_free(&avioCtx);
  }

  nxlog::pushf("YtAudioPlayer: playback loop ended (completed: %s, stopped: %s)",
               completed ? "YES" : "NO", stopToken->load() ? "YES" : "NO");

  bool isActualEnd = (durMs == 0 || currentPositionMs_.load() >= static_cast<uint32_t>(durMs * 0.80f));
  if (completed && isActualEnd && !stopToken->load() && generation == currentGeneration_.load()) {
    std::function<void()> cb;
    {
      std::lock_guard<std::mutex> lock(controlMutex_);
      cb = onComplete_;
    }
    if (cb) cb();
  } else if (!isActualEnd && completed) {
    nxlog::pushf("YtAudioPlayer: playback ended prematurely at %u ms (expected %u ms), not auto-advancing",
                 currentPositionMs_.load(), durMs);
  }
}

} // namespace switube
