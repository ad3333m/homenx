#include "pcm_output.hpp"

#include <array>
#include <cassert>

int main() {
    namespace pcm = switube::native::pcm;
    constexpr int sampleRate = 44100;
    std::atomic<bool> cancelled{false};
    std::array<std::int16_t, 441 * 2> decoded;
    std::array<std::int16_t, 441 * 2> mixed;
    decoded.fill(16000);
    mixed.fill(30000);

    pcm::setOutputRate(sampleRate);
    pcm::setVolume(0.5f);
    assert(pcm::enqueue(decoded.data(), 441, cancelled));
    assert(pcm::positionMs(sampleRate) == 0);

    pcm::setPaused(true);
    pcm::mix(mixed.data(), 441);
    assert(mixed.front() == 30000 && pcm::positionMs(sampleRate) == 0);

    pcm::setPaused(false);
    pcm::mix(mixed.data(), 441);
    assert(mixed.front() == 32767 && mixed.back() == 32767);
    assert(pcm::positionMs(sampleRate) == 10);

    mixed.fill(0);
    pcm::mix(mixed.data(), 441);
    assert(mixed.front() == 0 && pcm::positionMs(sampleRate) == 10);
    assert(pcm::drain(cancelled));

    pcm::reset();
    assert(pcm::positionMs(sampleRate) == 0);
    assert(pcm::enqueue(decoded.data(), 441, cancelled));
    pcm::setOutputRate(0);
    assert(!pcm::drain(cancelled));
    assert(!pcm::enqueue(decoded.data(), 441, cancelled));
}
