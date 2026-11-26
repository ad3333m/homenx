// Adapted by HomeNX from Switube by dl3g0, GPL-3.0-or-later.
#include "media_transfer.hpp"
#include "yt_audio_player.hpp"

#include "pcm_output.hpp"
#include <nxui/core/ThreadPool.hpp>
#include <thread>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <array>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
}

#include "NXConsoleLog.h"

namespace switube {

namespace {

constexpr size_t AVIO_BUFFER_SIZE = 32 * 1024;

Result startWorker(Thread& thread, std::function<void()> work, std::size_t stackSize) {
  auto context = std::make_unique<std::function<void()>>(std::move(work));
  Result rc = threadCreate(&thread, [](void* opaque) {
    std::unique_ptr<std::function<void()>> task(static_cast<std::function<void()>*>(opaque));
    nxui::moveToWorkerCores();
    (*task)();
  }, context.get(), nullptr, stackSize, 49, -2);
  if (R_FAILED(rc)) {
    thread = {};
    return rc;
  }
  // Hand off ownership before the new thread can execute.
  auto* task = context.release();
  rc = threadStart(&thread);
  if (R_FAILED(rc)) {
    delete task;
    threadClose(&thread);
    thread = {};
  }
  return rc;
}

void joinWorker(Thread& thread) {
  if (!thread.handle) return;
  threadWaitForExit(&thread);
  threadClose(&thread);
  thread = {};
}

struct BufferReader {
  std::vector<uint8_t> buffer;
  size_t readPos = 0;
  bool isDownloading = true;
  std::shared_ptr<std::atomic<bool>> stopToken;
  std::mutex mutex;
  std::string error;
};

int AvioRead(void* opaque, uint8_t* buf, int buf_size) {
  auto* reader = static_cast<BufferReader*>(opaque);
  while (!reader->stopToken->load()) {
    {
      std::lock_guard<std::mutex> lock(reader->mutex);
      if (reader->readPos < reader->buffer.size()) {
        int avail = static_cast<int>(reader->buffer.size() - reader->readPos);
        int toRead = std::min(buf_size, avail);
        std::memcpy(buf, reader->buffer.data() + reader->readPos, toRead);
        reader->readPos += toRead;
        if (reader->readPos == reader->buffer.size()) {
          reader->buffer.clear();
          reader->readPos = 0;
        }
        return toRead;
      }
      if (!reader->isDownloading)
        return reader->error.empty() ? AVERROR_EOF : AVERROR(EIO);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return AVERROR_EXIT;
}

} // namespace

YtAudioPlayer& YtAudioPlayer::getInstance() {
  static YtAudioPlayer instance;
  return instance;
}

YtAudioPlayer::~YtAudioPlayer() { stop(); }

void YtAudioPlayer::play(const std::string& url, uint32_t durationMs, uint64_t contentLength) {
  stop();
  auto token = std::make_shared<std::atomic<bool>>(false);
  {
    std::lock_guard lock(controlMutex_);
    lastError_.clear();
    currentToken_ = token;
    durationMs_ = durationMs;
    isPlaying_ = true;
  }
  const Result rc = startWorker(decoder_, [this, url, durationMs, contentLength, token] {
    try {
      audioWorker(url, durationMs, contentLength, token);
    } catch (const std::exception& ex) {
      std::lock_guard lock(controlMutex_);
      lastError_ = ex.what();
      isPlaying_ = isPaused_ = false;
      native::pcm::reset();
    }
  }, 512 * 1024);
  if (R_FAILED(rc)) {
    std::lock_guard lock(controlMutex_);
    isPlaying_ = false;
    lastError_ = "Could not start the stream decoder.";
    nxlog::pushf("Stream decoder thread creation: 0x%08X", rc);
  }
}

uint32_t YtAudioPlayer::getPositionMs() const {
  return native::pcm::positionMs(native::pcm::outputRate());
}

std::string YtAudioPlayer::takeError() {
  std::lock_guard<std::mutex> lock(controlMutex_);
  return std::exchange(lastError_, {});
}

void YtAudioPlayer::pause() {
  native::pcm::setPaused(true);
  isPaused_ = true;
  isPlaying_ = false;
}

void YtAudioPlayer::resume() {
  native::pcm::setPaused(false);
  isPaused_ = false;
  isPlaying_ = true;
}

void YtAudioPlayer::stop() {
  if (currentToken_) currentToken_->store(true);
  joinWorker(decoder_);
  isPlaying_ = isPaused_ = false;
  native::pcm::reset();
}

void YtAudioPlayer::setVolume(float volume) { native::pcm::setVolume(volume); }

void YtAudioPlayer::audioWorker(std::string url, uint32_t durMs, uint64_t contentLength,
                                std::shared_ptr<std::atomic<bool>> stopToken) {
  auto reader = std::make_shared<BufferReader>();
  reader->stopToken = stopToken;
  Thread transfer{};
  std::string playbackError;
  bool queuedAudio = false;
  const auto finish = [&](void*) {
    const bool cancelled = stopToken->exchange(true);
    joinWorker(transfer);
    std::lock_guard lock(controlMutex_);
    if (!cancelled) {
      if (!reader->error.empty()) lastError_ = std::move(reader->error);
      else if (!playbackError.empty()) lastError_ = std::move(playbackError);
      else if (!queuedAudio) lastError_ = "Could not decode the audio stream.";
    }
    isPlaying_ = isPaused_ = false;
    native::pcm::reset();
  };
  std::unique_ptr<void, decltype(finish)> status(this, finish);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!native::pcm::outputRate() && !stopToken->load() &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const int outputRate = native::pcm::outputRate();
  if (stopToken->load()) return;
  if (!outputRate) {
    playbackError = "HOME audio output is unavailable.";
    return;
  }

  std::unique_ptr<FILE, decltype(&std::fclose)> local(nullptr, std::fclose);
#ifdef HOMENX_STREAMING_MODULE
  if (url.starts_with("sdmc:/")) {
    local.reset(std::fopen(url.c_str(), "rb"));
    if (!local) { playbackError = "Could not read download"; return; }
  }
#endif
  const Result rc = local ? 0 : startWorker(transfer, [reader, url, stopToken, contentLength] {
    std::string failure;
    try {
      failure = native::transferAudio(url, contentLength, *stopToken,
          [reader, stopToken](std::span<const std::uint8_t> bytes) {
            std::unique_lock lock(reader->mutex);
            while (!reader->buffer.empty() && !stopToken->load()) {
              lock.unlock();
              std::this_thread::sleep_for(std::chrono::milliseconds(20));
              lock.lock();
            }
            if (stopToken->load()) return false;
            reader->buffer.insert(reader->buffer.end(), bytes.begin(), bytes.end());
            return true;
          });
    } catch (const std::exception& ex) {
      failure = ex.what();
    }
    std::lock_guard lock(reader->mutex);
    if (!stopToken->load()) reader->error = std::move(failure);
    reader->isDownloading = false;
  }, 512 * 1024);
  if (R_FAILED(rc)) {
    playbackError = "Could not start the stream connection.";
    nxlog::pushf("Audio transfer thread creation: 0x%08X", rc);
    return;
  }

  AVFormatContext* fmtCtx = nullptr;
  AVIOContext* avioCtx = nullptr;
  if (stopToken->load()) return;

  uint8_t* avioBuffer = static_cast<uint8_t*>(av_malloc(AVIO_BUFFER_SIZE));
  if (!avioBuffer) {
    nxlog::push("YtAudioPlayer error: av_malloc(AVIO_BUFFER_SIZE) failed");
    return;
  }
  if (local) {
    auto readFile = +[](void* arg, uint8_t* out, int size) -> int {
        FILE* file = static_cast<FILE*>(arg);
        int n = std::fread(out, 1, size, file);
        return n ? n : std::ferror(file) ? AVERROR(EIO) : AVERROR_EOF;
    };
    auto seekFile = +[](void* arg, int64_t off, int whence) -> int64_t {
        FILE* file = static_cast<FILE*>(arg);
        if (whence == AVSEEK_SIZE) {
            long pos = std::ftell(file);
            if (pos < 0 || std::fseek(file, 0, SEEK_END)) return AVERROR(EIO);
            long size = std::ftell(file);
            return std::fseek(file, pos, SEEK_SET) == 0 ? size : AVERROR(EIO);
        }
        return std::fseek(file, off, whence & ~AVSEEK_FORCE) == 0 ? std::ftell(file) : AVERROR(EIO);
    };
    avioCtx = avio_alloc_context(avioBuffer, AVIO_BUFFER_SIZE, 0, local.get(), readFile, nullptr, seekFile);
  } else {
    avioCtx = avio_alloc_context(avioBuffer, AVIO_BUFFER_SIZE, 0, reader.get(), AvioRead, nullptr, nullptr);
  }
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

  if (stopToken->load()) {
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

  if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
    nxlog::push("YtAudioPlayer error: avformat_find_stream_info failed");
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

  const AVCodec* codec = nullptr;
  int streamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (streamIdx < 0 || !codec) {
    nxlog::push("YtAudioPlayer error: no audio stream or codec found");
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

  AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
  if (!codecCtx) {
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
  if (avcodec_parameters_to_context(codecCtx, fmtCtx->streams[streamIdx]->codecpar) < 0 ||
      avcodec_open2(codecCtx, codec, nullptr) < 0) {
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

  // Match the actual HOME mixer rate and its signed stereo format.
  SwrContext* swrCtx = nullptr;
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
                                        outputRate,
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

  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  if (!pkt || !frame) {
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

  constexpr int MAX_OUT_SAMPLES = 8192;
  std::array<int16_t, MAX_OUT_SAMPLES * 2> pcmOut;

  nxlog::pushf("Audio decoding started (%d Hz -> %d Hz, codec: %s)",
               codecCtx->sample_rate, outputRate, codec->name);

  bool completed = false;
  while (!stopToken->load() && playbackError.empty()) {
    int ret = av_read_frame(fmtCtx, pkt);
    if (ret < 0) {
      if (ret == AVERROR_EOF) {
        completed = true;
      } else if (!stopToken->load()) {
        playbackError = "The audio stream was interrupted.";
      }
      break;
    }

    if (pkt->stream_index == streamIdx) {
      if (avcodec_send_packet(codecCtx, pkt) == 0) {
        while (avcodec_receive_frame(codecCtx, frame) == 0) {
          if (stopToken->load()) break;

          auto* outPtr = reinterpret_cast<uint8_t*>(pcmOut.data());
          const uint8_t** inData = frame->extended_data
                                       ? const_cast<const uint8_t**>(frame->extended_data)
                                       : const_cast<const uint8_t**>(frame->data);
          int outSamples = swr_convert(swrCtx,
                                       &outPtr,
                                       MAX_OUT_SAMPLES,
                                       inData,
                                       frame->nb_samples);
          if (outSamples > 0) {
            if (!native::pcm::enqueue(pcmOut.data(), outSamples, *stopToken)) {
              if (!stopToken->load()) playbackError = "HOME audio output closed.";
              break;
            }
            queuedAudio = true;
          }
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

  if (completed && queuedAudio && native::pcm::drain(*stopToken)) {
    const auto position = getPositionMs();
    if (durMs && position < durMs * 0.8f) {
      playbackError = "The audio stream ended early.";
      return;
    }
    std::function<void()> callback;
    {
      std::lock_guard lock(controlMutex_);
      callback = onComplete_;
    }
    if (callback && !stopToken->load()) callback();
  }
}

} // namespace switube
