#include "a2dp/sdp.hpp"

#include <algorithm>

// SDP assigned numbers/data elements verified against BlueZ lib/bluetooth/sdp.h.
// A2DP/AVDTP versions deliberately describe the baseline 1.2 profile, without
// advertising optional delay reporting. No external SDP implementation copied.
namespace a2dp {
SdpRecord make_sink_sdp_record(std::uint32_t record_handle, const char* name) {
    SdpRecord result;
    if (!name || record_handle < 0x10000) return result;
    std::size_t name_length = 0;
    while (name_length <= 63 && name[name_length]) {
        const auto c = static_cast<unsigned char>(name[name_length]);
        if (c < 0x20 || c > 0x7e) return result;
        ++name_length;
    }
    if (name_length > 63 || !name_length) return result;
    // Outer sequence uses a 16-bit length, patched after appending attributes.
    constexpr std::uint8_t prefix[]{
        0x36, 0, 0,
        0x09, 0, 0, 0x0a, 0, 0, 0, 0, // ServiceRecordHandle, uint32
        0x09, 0, 1, 0x35, 3, 0x19, 0x11, 0x0b, // Audio Sink service class
        0x09, 0, 4, 0x35, 16, // ProtocolDescriptorList
            0x35, 6, 0x19, 1, 0, 0x09, 0, 0x19, // L2CAP, PSM 25
            0x35, 6, 0x19, 0, 0x19, 0x09, 1, 2, // AVDTP 1.2
        0x09, 0, 5, 0x35, 3, 0x19, 0x10, 2, // Public browse group
        0x09, 0, 6, 0x35, 9, // LanguageBaseAttributeIDList
            0x09, 0x65, 0x6e, 0x09, 0, 0x6a, 0x09, 1, 0, // en, UTF-8, 0x100
        0x09, 0, 9, 0x35, 8, 0x35, 6, 0x19, 0x11, 0x0d, 0x09, 1, 2,
        0x09, 1, 0, 0x25 // ServiceName, string with 8-bit length
    };
    std::copy(std::begin(prefix), std::end(prefix), result.bytes.begin());
    result.size = sizeof(prefix);
    for (unsigned i = 0; i < 4; ++i)
        result.bytes[7 + i] = static_cast<std::uint8_t>(record_handle >> (24 - 8 * i));
    result.bytes[result.size++] = static_cast<std::uint8_t>(name_length);
    for (std::size_t i = 0; i < name_length; ++i)
        result.bytes[result.size++] = static_cast<std::uint8_t>(name[i]);
    // SupportedFeatures is required; zero claims no optional device categories.
    constexpr std::uint8_t features[]{0x09, 3, 0x11, 0x09, 0, 0};
    for (auto byte : features) result.bytes[result.size++] = byte;
    const auto payload_size = result.size - 3;
    result.bytes[1] = static_cast<std::uint8_t>(payload_size >> 8);
    result.bytes[2] = static_cast<std::uint8_t>(payload_size);
    return result;
}
} // namespace a2dp
