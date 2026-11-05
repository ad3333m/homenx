#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace a2dp {

// Portable acceptor for one SBC audio sink SEP. Feed complete signaling L2CAP
// SDUs, never HCI/ACL fragments. This class does not create Bluetooth channels.
enum class AvdtpState { Idle, Configured, Open, Streaming, Closing };
enum class AvdtpAction { None, Configure, AwaitMedia, Start, Suspend, CloseMedia, Abort };
enum class SignalDisposition { Response, IgnoredResponse, Malformed, FragmentUnsupported };

struct SbcConfiguration {
    std::array<std::uint8_t, 4> bytes{};
    std::uint32_t sample_rate() const;
    std::uint8_t channels() const { return 2; }
    std::uint8_t blocks() const;
    std::uint8_t subbands() const;
    bool joint_stereo() const { return (bytes[0] & 0x0f) == 1; }
    bool valid() const;
};

struct SignalResult {
    SignalDisposition disposition{SignalDisposition::Malformed};
    std::array<std::uint8_t, 32> bytes{};
    std::size_t size{};
    AvdtpAction action{AvdtpAction::None};
    bool accepted() const { return size >= 2 && (bytes[0] & 3) == 2; }
};

class AvdtpSink {
public:
    static constexpr std::uint8_t local_seid = 1;
    static constexpr std::size_t max_signal_size = 128;
    // 44.1/48 kHz, stereo/joint stereo; all standard SBC blocks/subbands/
    // allocations, bitpool 2..53. These are capabilities, not a configuration.
    static constexpr std::array<std::uint8_t, 4> sbc_capabilities{0x33, 0xff, 2, 53};

    SignalResult receive(const std::uint8_t* packet, std::size_t size);
    // Call only after a media channel from this signaling peer is established.
    bool media_connected();
    // Also use for transport failure/timeouts; drops the endpoint configuration.
    void reset();
    void media_disconnected() { reset(); }
    AvdtpState state() const { return state_; }
    const SbcConfiguration& configuration() const { return configuration_; }
    std::uint8_t remote_seid() const { return remote_seid_; }
    bool media_ready() const { return media_ready_; }

private:
    AvdtpState state_{AvdtpState::Idle};
    SbcConfiguration configuration_{};
    std::uint8_t remote_seid_{};
    bool media_ready_{};
};

const char* avdtp_state_name(AvdtpState state);

} // namespace a2dp
