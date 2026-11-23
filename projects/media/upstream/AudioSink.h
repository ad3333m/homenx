#pragma once

#include <cstdint>
#include <cstdlib>

class AudioSink {
 public:
  AudioSink() {}
  virtual ~AudioSink() {}
  virtual void feedPCMFrames(const uint8_t* buffer, size_t bytes) = 0;
  virtual void flush() {}
  virtual void volumeChanged(uint16_t volume) {}
  virtual bool setParams(uint32_t sampleRate, uint8_t channelCount,
                         uint8_t bitDepth) {
    return false;
  }
  virtual inline bool setRate(uint16_t sampleRate) {
    return setParams(sampleRate, 2, 16);
  }
  bool softwareVolumeControl = true;
  bool usign = false;
};
