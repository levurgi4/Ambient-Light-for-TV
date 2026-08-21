#include "protocol_dispatcher.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_timer.h"

namespace ambilight::firmware {
namespace {

constexpr std::uint8_t kFirmwareVersionMajor = 0;
constexpr std::uint8_t kFirmwareVersionMinor = 1;
constexpr std::uint8_t kFirmwareVersionPatch = 1;

constexpr std::uint16_t kSupportedPacketFlags =
    protocol::PacketFlagAckRequested | protocol::PacketFlagPersist;

[[nodiscard]] bool has_flag(
    const std::uint16_t flags,
    const protocol::PacketFlag flag) {
    return (flags & static_cast<std::uint16_t>(flag)) != 0;
}

}  // namespace

ProtocolDispatcher::ProtocolDispatcher(
    DeviceState& state,
    ByteStreamWriter& writer)
    : state_(state), writer_(writer) {
    incoming_pixels_.reserve(kDefaultLedCount);
}

void ProtocolDispatcher::consume(
    const std::span<const std::uint8_t> bytes) {
    decoder_.push(bytes, [this](const PacketView& packet) {
        dispatch(packet);
    });
}

void ProtocolDispatcher::dispatch(const PacketView& packet) {
    // These are device-to-host response types. Silently ignoring them before
    // all other validation avoids an error-response loop if the wrong endpoint
    // sends one.
    switch (packet.type) {
        case MessageType::DeviceInfo:
        case MessageType::Pong:
        case MessageType::State:
        case MessageType::Ack:
        case MessageType::Error:
            state_.mark_packet_rejected();
            return;
        default:
            break;
    }

    if (packet.version != protocol::kProtocolVersion) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::UnsupportedVersion);
        return;
    }

    if ((packet.flags & ~kSupportedPacketFlags) != 0) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::InvalidPayload);
        return;
    }

    switch (packet.type) {
        case MessageType::Hello:
            handle_hello(packet);
            break;
        case MessageType::Ping:
            handle_ping(packet);
            break;
        case MessageType::GetState:
            handle_get_state(packet);
            break;
        case MessageType::SetStripConfig:
            handle_strip_config(packet);
            break;
        case MessageType::SetMode:
            handle_mode(packet);
            break;
        case MessageType::SetBrightness:
            handle_brightness(packet);
            break;
        case MessageType::SetEffect:
            handle_effect(packet);
            break;
        case MessageType::LedFrame:
            handle_led_frame(packet);
            break;
        case MessageType::SaveConfig:
            state_.mark_packet_rejected();
            send_error(packet, protocol::ErrorCode::UnsupportedMessage);
            break;

        case MessageType::DeviceInfo:
        case MessageType::Pong:
        case MessageType::State:
        case MessageType::Ack:
        case MessageType::Error:
            break;
        default:
            state_.mark_packet_rejected();
            send_error(packet, protocol::ErrorCode::UnsupportedMessage);
            break;
    }
}

void ProtocolDispatcher::handle_hello(const PacketView& packet) {
    if (!packet.payload.empty() ||
        packet.flags != protocol::PacketFlagNone) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::InvalidPayload);
        return;
    }

    state_.begin_host_session();
    const auto config = state_.config();
    // Capabilities describe the binary protocol implemented by this firmware,
    // not whether this particular build currently drives a GPIO. Safe no-op
    // builds must remain connectable for bring-up; STATE exposes the actual
    // output through OutputActive and lastAppliedSequence.
    constexpr std::uint32_t capabilities =
        protocol::CapabilityRgb8Frames |
        protocol::CapabilityStandaloneEffects |
        protocol::CapabilityCurrentLimit |
        protocol::CapabilityAmbientLease;
    protocol::DeviceInfo info{
        .firmwareMajor = kFirmwareVersionMajor,
        .firmwareMinor = kFirmwareVersionMinor,
        .firmwarePatch = kFirmwareVersionPatch,
        .protocolVersion = protocol::kProtocolVersion,
        .maxLedCount = kDefaultLedCount,
        .configuredLedCount = config.led_count,
        .capabilities = capabilities,
        .uptimeMilliseconds =
            static_cast<std::uint32_t>(milliseconds_since_boot()),
    };
    std::array<protocol::Byte, protocol::kDeviceInfoPayloadSize> payload{};
    if (!protocol::encodeDeviceInfo(info, payload) ||
        !send_packet(MessageType::DeviceInfo, packet.sequence, payload)) {
        send_error(packet, protocol::ErrorCode::Internal);
    }
}

void ProtocolDispatcher::handle_ping(const PacketView& packet) {
    if (packet.flags != protocol::PacketFlagNone) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::InvalidPayload);
        return;
    }
    if (!send_packet(MessageType::Pong, packet.sequence, packet.payload)) {
        send_error(packet, protocol::ErrorCode::Internal);
    }
}

void ProtocolDispatcher::handle_get_state(const PacketView& packet) {
    if (!packet.payload.empty() ||
        packet.flags != protocol::PacketFlagNone) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::InvalidPayload);
        return;
    }

    const auto snapshot = make_state_snapshot();
    std::array<protocol::Byte, protocol::kStatePayloadSize> payload{};
    if (!protocol::encodeState(snapshot, payload) ||
        !send_packet(MessageType::State, packet.sequence, payload)) {
        send_error(packet, protocol::ErrorCode::Internal);
    }
}

void ProtocolDispatcher::handle_strip_config(const PacketView& packet) {
    if (has_flag(packet.flags, protocol::PacketFlagPersist)) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::UnsupportedMessage);
        return;
    }

    protocol::StripConfig wire_config;
    if (!protocol::decodeStripConfig(packet.payload, wire_config)) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::InvalidPayload);
        return;
    }

    DeviceConfig config{
        .strip_type = StripType::ws2815,
        .color_order = from_protocol(wire_config.colorOrder),
        .led_count = wire_config.ledCount,
        .top_count = wire_config.topCount,
        .right_count = wire_config.rightCount,
        .bottom_count = wire_config.bottomCount,
        .left_count = wire_config.leftCount,
        .start_offset = wire_config.startOffset,
        .brightness_limit = wire_config.brightnessLimit,
        .current_limit_milliamps = wire_config.currentLimitMilliamps,
        .ambient_timeout_milliseconds =
            wire_config.ambientTimeoutMilliseconds,
        .color_calibration = {
            wire_config.redCalibration,
            wire_config.greenCalibration,
            wire_config.blueCalibration,
        },
        .reversed = wire_config.reversed,
    };

    if (!state_.set_config(config)) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::InvalidPayload);
        return;
    }
    send_ack_if_requested(packet);
}

void ProtocolDispatcher::handle_mode(const PacketView& packet) {
    if (has_flag(packet.flags, protocol::PacketFlagPersist)) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::UnsupportedMessage);
        return;
    }

    protocol::OperatingMode wire_mode;
    if (!protocol::decodeOperatingMode(packet.payload, wire_mode)) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::InvalidPayload);
        return;
    }

    const auto current = state_.status(milliseconds_since_boot());
    auto mode = OperatingMode::off;
    switch (wire_mode) {
        case protocol::OperatingMode::Off:
            mode = OperatingMode::off;
            break;
        case protocol::OperatingMode::Ambilight:
            mode = OperatingMode::ambient;
            break;
        case protocol::OperatingMode::Solid:
            mode = OperatingMode::solid;
            break;
        case protocol::OperatingMode::Effect:
            switch (current.selected_effect_mode) {
                case OperatingMode::breathing:
                case OperatingMode::wave:
                case OperatingMode::rainbow:
                    mode = current.selected_effect_mode;
                    break;
                default:
                    mode = OperatingMode::breathing;
                    break;
            }
            break;
    }

    if (!state_.set_mode(mode, current.effect)) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::InvalidState);
        return;
    }
    send_ack_if_requested(packet);
}

void ProtocolDispatcher::handle_brightness(const PacketView& packet) {
    if (has_flag(packet.flags, protocol::PacketFlagPersist)) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::UnsupportedMessage);
        return;
    }

    protocol::Byte brightness = 0;
    if (!protocol::decodeBrightness(packet.payload, brightness) ||
        !state_.set_brightness(brightness)) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::InvalidPayload);
        return;
    }
    send_ack_if_requested(packet);
}

void ProtocolDispatcher::handle_effect(const PacketView& packet) {
    if (has_flag(packet.flags, protocol::PacketFlagPersist)) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::UnsupportedMessage);
        return;
    }

    protocol::EffectConfig wire_effect;
    if (!protocol::decodeEffectConfig(packet.payload, wire_effect)) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::InvalidPayload);
        return;
    }

    EffectSettings effect{
        .color = {
            wire_effect.red,
            wire_effect.green,
            wire_effect.blue,
        },
        .intensity = wire_effect.intensity,
        .speed_percent = static_cast<std::uint16_t>(
            (wire_effect.speedPermille + 5U) / 10U),
    };
    const auto mode = from_protocol_effect(wire_effect.effect);
    if (!state_.set_mode(mode, effect)) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::InvalidPayload);
        return;
    }
    send_ack_if_requested(packet);
}

void ProtocolDispatcher::handle_led_frame(const PacketView& packet) {
    if (packet.flags != protocol::PacketFlagNone) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::InvalidPayload);
        return;
    }

    const auto frame = protocol::decodeLedFramePayload(packet.payload);
    const auto config = state_.config();
    if (!frame.valid || frame.ledCount != config.led_count) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::InvalidPayload);
        return;
    }

    incoming_pixels_.resize(frame.ledCount);
    for (std::size_t index = 0; index < incoming_pixels_.size(); ++index) {
        const auto byte_offset = index * 3U;
        incoming_pixels_[index] = {
            frame.pixels[byte_offset],
            frame.pixels[byte_offset + 1U],
            frame.pixels[byte_offset + 2U],
        };
    }

    if (!state_.accept_ambient_frame(
            packet.sequence,
            incoming_pixels_,
            milliseconds_since_boot())) {
        state_.mark_packet_rejected();
        send_error(packet, protocol::ErrorCode::InvalidState);
    }
    // LED_FRAME is intentionally never ACKed. GET_STATE exposes the latest
    // received/applied sequence and keeps the 60 FPS path free of TX traffic.
}

bool ProtocolDispatcher::send_packet(
    const MessageType type,
    const std::uint32_t sequence,
    const std::span<const protocol::Byte> payload) {
    const auto encoded = protocol::encodePacket(
        type,
        protocol::PacketFlagNone,
        sequence,
        payload,
        wire_buffer_);
    return encoded.ok &&
           writer_.write(std::span<const protocol::Byte>(
               wire_buffer_.data(),
               encoded.bytesWritten));
}

void ProtocolDispatcher::send_ack_if_requested(const PacketView& packet) {
    if (!has_flag(packet.flags, protocol::PacketFlagAckRequested)) {
        return;
    }

    const protocol::AckPayload ack{
        .acknowledgedType = packet.type,
        .status = 0,
    };
    std::array<protocol::Byte, protocol::kAckPayloadSize> payload{};
    if (!protocol::encodeAck(ack, payload) ||
        !send_packet(MessageType::Ack, packet.sequence, payload)) {
        send_error(packet, protocol::ErrorCode::Internal);
    }
}

void ProtocolDispatcher::send_error(
    const PacketView& packet,
    const protocol::ErrorCode code,
    const std::uint8_t detail) {
    const protocol::ErrorPayload error{
        .code = code,
        .failedType = packet.type,
        .detail = detail,
    };
    std::array<protocol::Byte, protocol::kErrorPayloadSize> payload{};
    if (protocol::encodeError(error, payload)) {
        static_cast<void>(
            send_packet(MessageType::Error, packet.sequence, payload));
    }
}

protocol::StateSnapshot ProtocolDispatcher::make_state_snapshot() const {
    const auto now = milliseconds_since_boot();
    const auto device_status = state_.status(now);
    const auto config = state_.config();

    std::uint8_t flags = protocol::StateFlagNone;
    if (device_status.physical_output_present &&
        device_status.output_healthy &&
        device_status.mode != OperatingMode::off &&
        config.brightness_limit != 0 &&
        (config.color_calibration.red != 0 ||
         config.color_calibration.green != 0 ||
         config.color_calibration.blue != 0)) {
        flags |= protocol::StateFlagOutputActive;
    }
    if (device_status.ambient_lease_active) {
        flags |= protocol::StateFlagAmbientLeaseActive;
    }

    return {
        .mode = to_protocol(device_status.mode),
        .effect = to_protocol_effect(
            device_status.selected_effect_mode),
        .brightness = config.brightness_limit,
        .flags = flags,
        .ledCount = config.led_count,
        .currentLimitMilliamps = config.current_limit_milliamps,
        .lastReceivedSequence =
            device_status.last_received_frame_sequence,
        .lastAppliedSequence =
            device_status.last_applied_frame_sequence,
        .crcErrors = decoder_.stats().crcErrors,
        .droppedFrames = device_status.dropped_frames,
        .uptimeMilliseconds = static_cast<std::uint32_t>(now),
    };
}

std::uint64_t ProtocolDispatcher::milliseconds_since_boot() {
    return static_cast<std::uint64_t>(esp_timer_get_time()) / 1000ULL;
}

ColorOrder ProtocolDispatcher::from_protocol(
    const protocol::ColorOrder order) {
    switch (order) {
        case protocol::ColorOrder::Rgb:
            return ColorOrder::rgb;
        case protocol::ColorOrder::Rbg:
            return ColorOrder::rbg;
        case protocol::ColorOrder::Grb:
            return ColorOrder::grb;
        case protocol::ColorOrder::Gbr:
            return ColorOrder::gbr;
        case protocol::ColorOrder::Brg:
            return ColorOrder::brg;
        case protocol::ColorOrder::Bgr:
            return ColorOrder::bgr;
    }
    return ColorOrder::grb;
}

protocol::OperatingMode ProtocolDispatcher::to_protocol(
    const OperatingMode mode) {
    switch (mode) {
        case OperatingMode::off:
            return protocol::OperatingMode::Off;
        case OperatingMode::ambient:
            return protocol::OperatingMode::Ambilight;
        case OperatingMode::solid:
            return protocol::OperatingMode::Solid;
        case OperatingMode::breathing:
        case OperatingMode::wave:
        case OperatingMode::rainbow:
            return protocol::OperatingMode::Effect;
    }
    return protocol::OperatingMode::Off;
}

protocol::EffectType ProtocolDispatcher::to_protocol_effect(
    const OperatingMode mode) {
    switch (mode) {
        case OperatingMode::solid:
            return protocol::EffectType::Solid;
        case OperatingMode::wave:
            return protocol::EffectType::Wave;
        case OperatingMode::rainbow:
            return protocol::EffectType::Rainbow;
        case OperatingMode::off:
        case OperatingMode::ambient:
        case OperatingMode::breathing:
            return protocol::EffectType::Breathing;
    }
    return protocol::EffectType::Breathing;
}

OperatingMode ProtocolDispatcher::from_protocol_effect(
    const protocol::EffectType effect) {
    switch (effect) {
        case protocol::EffectType::Solid:
            return OperatingMode::solid;
        case protocol::EffectType::Breathing:
            return OperatingMode::breathing;
        case protocol::EffectType::Wave:
            return OperatingMode::wave;
        case protocol::EffectType::Rainbow:
            return OperatingMode::rainbow;
    }
    return OperatingMode::breathing;
}

}  // namespace ambilight::firmware
