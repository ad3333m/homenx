#include "a2dp/avdtp.hpp"

#include <algorithm>

// Wire constants cross-checked against BlueZ profiles/audio/avdtp.{c,h}
// and a2dp-codecs.h. No BlueZ implementation is included or linked here.
namespace a2dp {
namespace {
constexpr std::uint8_t discover = 1, get_caps = 2, set_config = 3,
    get_config = 4, reconfigure = 5, open = 6, start = 7, close = 8,
    suspend = 9, abort_signal = 10, get_all_caps = 12;
constexpr std::uint8_t bad_header = 0x01, bad_length = 0x11,
    bad_seid = 0x12, in_use = 0x13, not_in_use = 0x14,
    bad_category = 0x17, bad_payload = 0x18, unsupported = 0x19,
    invalid_caps = 0x1a, bad_transport = 0x23,
    unsupported_config = 0x29, bad_state = 0x31;

bool one_bit(std::uint8_t value) {
    return value && !(value & (value - 1));
}

SignalResult response(std::uint8_t header, std::uint8_t signal,
                      std::uint8_t type = 2) {
    SignalResult result;
    result.disposition = SignalDisposition::Response;
    result.bytes[0] = (header & 0xf0) | type;
    result.bytes[1] = signal;
    result.size = 2;
    return result;
}

SignalResult reject(std::uint8_t header, std::uint8_t signal,
                    std::uint8_t error, std::uint8_t extra = 0) {
    auto result = response(header, signal, 3);
    // Configuration rejects identify the service category. Start/Suspend
    // rejects identify the first failing ACP SEID (already wire encoded).
    if (signal == set_config || signal == reconfigure ||
        signal == start || signal == suspend)
        result.bytes[result.size++] = extra;
    result.bytes[result.size++] = error;
    return result;
}

void append_caps(SignalResult& result, const std::array<std::uint8_t, 4>& sbc) {
    constexpr std::uint8_t prefix[]{1, 0, 7, 6, 0, 0};
    for (auto byte : prefix) result.bytes[result.size++] = byte;
    for (auto byte : sbc) result.bytes[result.size++] = byte;
}
} // namespace

bool SbcConfiguration::valid() const {
    const auto frequencies = static_cast<std::uint8_t>(bytes[0] & 0xf0);
    const auto modes = static_cast<std::uint8_t>(bytes[0] & 0x0f);
    return one_bit(frequencies) && !(frequencies & ~0x30) &&
           one_bit(modes) && !(modes & ~0x03) &&
           one_bit(bytes[1] & 0xf0) && one_bit(bytes[1] & 0x0c) &&
           one_bit(bytes[1] & 0x03) && bytes[2] >= 2 &&
           bytes[2] <= bytes[3] && bytes[3] <= 53;
}

std::uint32_t SbcConfiguration::sample_rate() const {
    return (bytes[0] & 0xf0) == 0x20 ? 44100 :
           (bytes[0] & 0xf0) == 0x10 ? 48000 : 0;
}

std::uint8_t SbcConfiguration::blocks() const {
    switch (bytes[1] & 0xf0) {
    case 0x80: return 4;
    case 0x40: return 8;
    case 0x20: return 12;
    case 0x10: return 16;
    default: return 0;
    }
}

std::uint8_t SbcConfiguration::subbands() const {
    return (bytes[1] & 0x0c) == 0x08 ? 4 :
           (bytes[1] & 0x0c) == 0x04 ? 8 : 0;
}

bool AvdtpSink::media_connected() {
    if (state_ != AvdtpState::Open || media_ready_) return false;
    media_ready_ = true;
    return true;
}

void AvdtpSink::reset() {
    state_ = AvdtpState::Idle;
    configuration_ = {};
    remote_seid_ = 0;
    media_ready_ = false;
}

SignalResult AvdtpSink::receive(const std::uint8_t* packet, std::size_t size) {
    if (!packet || !size) return {};
    if (packet[0] & 3) {
        SignalResult result;
        result.disposition = SignalDisposition::IgnoredResponse;
        return result;
    }
    const auto packet_type = (packet[0] >> 2) & 3;
    if (packet_type) {
        // This prototype accepts single signaling packets only. A START has a
        // signal ID and can be rejected. CONTINUE/END do not, so drop them.
        // Never interpret continuation payload as a command or change state.
        SignalResult result;
        if (packet_type == 1 && size >= 3)
            result = reject(packet[0], packet[2] & 0x3f, bad_header);
        result.disposition = SignalDisposition::FragmentUnsupported;
        return result;
    }
    if (size < 2) return {};
    const auto signal = static_cast<std::uint8_t>(packet[1] & 0x3f);
    if (size > max_signal_size)
        return reject(packet[0], signal, bad_length);
    // Reserved fields are ignored on reception as specified by AVDTP.
    const auto* payload = packet + 2;
    const auto length = size - 2;
    auto result = response(packet[0], signal);

    if (signal == discover) {
        if (length) return reject(packet[0], signal, bad_length);
        result.bytes[result.size++] = static_cast<std::uint8_t>(
            (local_seid << 2) | (state_ != AvdtpState::Idle ? 2 : 0));
        result.bytes[result.size++] = 0x08; // Audio, TSEP=sink.
        return result;
    }
    if (signal != get_caps && signal != get_all_caps && signal != set_config &&
        signal != get_config && signal != reconfigure && signal != open &&
        signal != start && signal != close && signal != suspend &&
        signal != abort_signal)
        return response(packet[0], signal, 1); // General reject: unknown signal.

    if (!length) return reject(packet[0], signal, bad_length);
    const auto seid = static_cast<std::uint8_t>(payload[0] >> 2);
    if (seid != local_seid)
        return reject(packet[0], signal, bad_seid,
                      (signal == start || signal == suspend) ? payload[0] & 0xfc : 0);

    if (signal == set_config) {
        if (length < 2) return reject(packet[0], signal, bad_length);
        if (state_ != AvdtpState::Idle) return reject(packet[0], signal, in_use);
        const auto peer_seid = static_cast<std::uint8_t>(payload[1] >> 2);
        if (!peer_seid || peer_seid > 0x3e)
            return reject(packet[0], signal, bad_payload);
        bool transport = false, codec = false;
        SbcConfiguration selected;
        std::size_t offset = 2;
        while (offset < length) {
            const auto category = payload[offset];
            if (length - offset < 2)
                return reject(packet[0], signal, bad_length, category);
            const auto cap_length = payload[offset + 1];
            offset += 2;
            if (cap_length > length - offset)
                return reject(packet[0], signal, bad_length, category);
            if (category == 1) {
                if (transport) return reject(packet[0], signal, invalid_caps, category);
                if (cap_length) return reject(packet[0], signal, bad_transport, category);
                transport = true;
            } else if (category == 7) {
                if (codec) return reject(packet[0], signal, invalid_caps, category);
                if (cap_length != 6)
                    return reject(packet[0], signal, bad_length, category);
                if ((payload[offset] & 0xf0) || payload[offset + 1])
                    return reject(packet[0], signal, unsupported_config, category);
                std::copy_n(payload + offset + 2, 4, selected.bytes.begin());
                if (!selected.valid())
                    return reject(packet[0], signal, unsupported_config, category);
                codec = true;
            } else {
                return reject(packet[0], signal, bad_category, category);
            }
            offset += cap_length;
        }
        if (!transport || !codec)
            return reject(packet[0], signal, invalid_caps, !transport ? 1 : 7);
        configuration_ = selected;
        remote_seid_ = peer_seid;
        state_ = AvdtpState::Configured;
        result.action = AvdtpAction::Configure;
        return result;
    }

    if (signal == reconfigure)
        return reject(packet[0], signal, unsupported);

    if (signal == start || signal == suspend) {
        // Validate the entire list before changing any state. Only one SEP is
        // implemented; a second identifier (even a duplicate) cannot succeed.
        if (length != 1)
            return reject(packet[0], signal,
                          length > 1 && (payload[1] >> 2) != local_seid ? bad_seid : bad_payload,
                          length > 1 ? payload[1] & 0xfc : 0);
        if (signal == start) {
            if (state_ != AvdtpState::Open || !media_ready_)
                return reject(packet[0], signal, bad_state, local_seid << 2);
            state_ = AvdtpState::Streaming;
            result.action = AvdtpAction::Start;
        } else {
            if (state_ != AvdtpState::Streaming)
                return reject(packet[0], signal, bad_state, local_seid << 2);
            state_ = AvdtpState::Open;
            result.action = AvdtpAction::Suspend;
        }
        return result;
    }

    if (length != 1) return reject(packet[0], signal, bad_length);
    switch (signal) {
    case get_caps:
    case get_all_caps:
        append_caps(result, sbc_capabilities);
        break;
    case get_config:
        if (state_ == AvdtpState::Idle)
            return reject(packet[0], signal, not_in_use);
        append_caps(result, configuration_.bytes);
        break;
    case open:
        if (state_ != AvdtpState::Configured)
            return reject(packet[0], signal, bad_state);
        state_ = AvdtpState::Open;
        result.action = AvdtpAction::AwaitMedia;
        break;
    case close:
        if (state_ != AvdtpState::Open && state_ != AvdtpState::Streaming)
            return reject(packet[0], signal, bad_state);
        state_ = AvdtpState::Closing;
        media_ready_ = false;
        result.action = AvdtpAction::CloseMedia;
        break;
    case abort_signal:
        reset();
        result.action = AvdtpAction::Abort;
        break;
    default:
        break;
    }
    return result;
}

const char* avdtp_state_name(AvdtpState state) {
    switch (state) {
    case AvdtpState::Idle: return "idle";
    case AvdtpState::Configured: return "configured";
    case AvdtpState::Open: return "open";
    case AvdtpState::Streaming: return "streaming";
    case AvdtpState::Closing: return "closing";
    }
    return "unknown";
}
} // namespace a2dp
