// HomeNX foreground PCM output through the existing SDL_mixer callback.
#include "pcm_output.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace switube::native::pcm {
namespace {

constexpr std::size_t Capacity = 16384;
std::array<std::int16_t, Capacity * 2> buffer;
std::mutex mutex;
std::condition_variable changed;
std::atomic<int> rate{0};
std::atomic<float> gain{0.8f};
std::atomic<std::uint64_t> mixedFrames{0};
std::size_t readAt = 0, queued = 0;
bool paused = false;

}

void setOutputRate(int value) {
    std::lock_guard lock(mutex);
    rate = value;
    queued = 0;
    changed.notify_all();
}

int outputRate() { return rate; }

void reset() {
    std::lock_guard lock(mutex);
    readAt = queued = 0;
    mixedFrames = 0;
    paused = false;
    changed.notify_all();
}

void setPaused(bool value) {
    std::lock_guard lock(mutex);
    paused = value;
}

void setVolume(float value) { gain = std::clamp(value, 0.f, 1.f); }

bool enqueue(const std::int16_t* samples, std::size_t frames, const std::atomic<bool>& cancelled) {
    std::unique_lock lock(mutex);
    while (frames && rate && !cancelled) {
        if (queued == Capacity) {
            changed.wait_for(lock, std::chrono::milliseconds(50));
            continue;
        }
        const auto writeAt = (readAt + queued) % Capacity;
        const auto count = std::min({frames, Capacity - queued, Capacity - writeAt});
        std::copy_n(samples, count * 2, buffer.data() + writeAt * 2);
        samples += count * 2;
        frames -= count;
        queued += count;
    }
    return frames == 0;
}

bool drain(const std::atomic<bool>& cancelled) {
    std::unique_lock lock(mutex);
    while (queued && rate && !cancelled)
        changed.wait_for(lock, std::chrono::milliseconds(50));
    return rate && !cancelled;
}

std::uint32_t positionMs(int outputRate) {
    return outputRate > 0 ? mixedFrames.load() * 1000 / outputRate : 0;
}

void mix(std::int16_t* samples, std::size_t frames) {
    std::lock_guard lock(mutex);
    if (paused || !rate || !queued) return;
    const auto count = std::min(frames, queued);
    const float volume = gain.load();
    for (std::size_t i = 0; i < count; ++i) {
        const auto source = ((readAt + i) % Capacity) * 2;
        for (std::size_t channel = 0; channel < 2; ++channel) {
            auto& sample = samples[i * 2 + channel];
            sample = std::clamp(sample + static_cast<int>(buffer[source + channel] * volume), -32768, 32767);
        }
    }
    readAt = (readAt + count) % Capacity;
    queued -= count;
    mixedFrames += count;
    changed.notify_one();
}

}
