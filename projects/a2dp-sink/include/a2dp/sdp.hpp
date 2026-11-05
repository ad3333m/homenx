#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace a2dp {

struct SdpRecord {
    std::array<std::uint8_t, 192> bytes{};
    std::size_t size{};
};

// Builds the SDP data-element sequence describing an Audio Sink service.
// This is NOT an SDP server and does NOT register it with Horizon or broadcast
// it. A future stack adapter must register these attributes and listen on
// AVDTP PSM 0x0019. The owning SDP server must allocate record_handle.
// Name is limited to 63 printable ASCII bytes; invalid input returns size=0.
SdpRecord make_sink_sdp_record(std::uint32_t record_handle,
                               const char* name = "HomeNX Bluetooth Audio Receiver");

} // namespace a2dp
