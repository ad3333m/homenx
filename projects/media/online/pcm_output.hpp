#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace switube::native::pcm {

void setOutputRate(int rate);
int outputRate();
void reset();
void setPaused(bool paused);
void setVolume(float volume);
bool enqueue(const std::int16_t* samples, std::size_t frames, const std::atomic<bool>& cancelled);
bool drain(const std::atomic<bool>& cancelled);
std::uint32_t positionMs(int rate);
void mix(std::int16_t* samples, std::size_t frames);

}
