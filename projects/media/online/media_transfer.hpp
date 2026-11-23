#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace switube::native {

// The sink receives only complete, validated ranges, in order.
std::string transferAudio(const std::string& url, std::uint64_t expectedBytes,
    const std::atomic<bool>& cancelled,
    const std::function<bool(std::span<const std::uint8_t>)>& consume);

} // namespace switube::native
