#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <ambilight/protocol/protocol.hpp>

#include "byte_stream.hpp"
#include "device_state.hpp"

namespace ambilight::firmware {

class ProtocolDispatcher final : public ByteStreamSink {
public:
    ProtocolDispatcher(DeviceState& state, ByteStreamWriter& writer);

    void consume(std::span<const std::uint8_t> bytes) override;

private:
    using MessageType = protocol::MessageType;
    using PacketView = protocol::PacketView;

    void dispatch(const PacketView& packet);
    void handle_hello(const PacketView& packet);
    void handle_ping(const PacketView& packet);
    void handle_get_state(const PacketView& packet);
    void handle_strip_config(const PacketView& packet);
    void handle_mode(const PacketView& packet);
    void handle_brightness(const PacketView& packet);
    void handle_effect(const PacketView& packet);
    void handle_led_frame(const PacketView& packet);

    [[nodiscard]] bool send_packet(
        MessageType type,
        std::uint32_t sequence,
        std::span<const protocol::Byte> payload);
    void send_ack_if_requested(const PacketView& packet);
    void send_error(
        const PacketView& packet,
        protocol::ErrorCode code,
        std::uint8_t detail = 0);

    [[nodiscard]] protocol::StateSnapshot make_state_snapshot() const;
    [[nodiscard]] static std::uint64_t milliseconds_since_boot();
    [[nodiscard]] static ColorOrder from_protocol(
        protocol::ColorOrder order);
    [[nodiscard]] static protocol::OperatingMode to_protocol(
        OperatingMode mode);
    [[nodiscard]] static protocol::EffectType to_protocol_effect(
        OperatingMode mode);
    [[nodiscard]] static OperatingMode from_protocol_effect(
        protocol::EffectType effect);

    DeviceState& state_;
    ByteStreamWriter& writer_;
    protocol::StreamDecoder decoder_;
    std::vector<Rgb> incoming_pixels_;
    std::array<protocol::Byte, protocol::kMaxWirePacketSize> wire_buffer_{};
};

}  // namespace ambilight::firmware
