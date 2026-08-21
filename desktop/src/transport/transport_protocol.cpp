#include "transport_protocol.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <utility>

namespace ambilight::transport {

namespace {

[[nodiscard]] std::span<const protocol::Byte> bytesOf(
    const QByteArrayView bytes) noexcept
{
    return {
        reinterpret_cast<const protocol::Byte*>(bytes.data()),
        static_cast<std::size_t>(bytes.size()),
    };
}

template<std::size_t Size, typename Encoder>
[[nodiscard]] std::optional<QByteArray> encodePayloadPacket(
    const protocol::MessageType type,
    const quint32 sequence,
    Encoder&& encoder)
{
    std::array<protocol::Byte, Size> payload{};
    if (!encoder(std::span<protocol::Byte>(payload))) {
        return std::nullopt;
    }
    return encodeProtocolPacket(
        type,
        protocol::PacketFlagAckRequested,
        sequence,
        QByteArrayView(
            reinterpret_cast<const char*>(payload.data()),
            static_cast<qsizetype>(payload.size())));
}

} // namespace

std::optional<QByteArray> encodeProtocolPacket(
    const protocol::MessageType type,
    const quint16 flags,
    const quint32 sequence,
    const QByteArrayView payload)
{
    if (payload.size() < 0
        || static_cast<std::size_t>(payload.size())
            > protocol::kMaxPayloadSize) {
        return std::nullopt;
    }

    std::array<protocol::Byte, protocol::kMaxWirePacketSize> wire{};
    const auto result = protocol::encodePacket(
        type,
        flags,
        sequence,
        bytesOf(payload),
        wire);
    if (!result.ok) {
        return std::nullopt;
    }
    return QByteArray(
        reinterpret_cast<const char*>(wire.data()),
        static_cast<qsizetype>(result.bytesWritten));
}

std::optional<QByteArray> encodeHelloPacket(const quint32 sequence)
{
    return encodeProtocolPacket(
        protocol::MessageType::Hello,
        protocol::PacketFlagNone,
        sequence);
}

std::optional<QByteArray> encodeGetStatePacket(const quint32 sequence)
{
    return encodeProtocolPacket(
        protocol::MessageType::GetState,
        protocol::PacketFlagNone,
        sequence);
}

std::optional<QByteArray> encodePingPacket(
    const quint32 sequence,
    const QByteArrayView payload)
{
    return encodeProtocolPacket(
        protocol::MessageType::Ping,
        protocol::PacketFlagNone,
        sequence,
        payload);
}

std::optional<QByteArray> encodeStripConfigurationPacket(
    const quint32 sequence,
    const protocol::StripConfig& configuration)
{
    return encodePayloadPacket<protocol::kStripConfigPayloadSize>(
        protocol::MessageType::SetStripConfig,
        sequence,
        [&configuration](const std::span<protocol::Byte> payload) {
            return protocol::encodeStripConfig(configuration, payload);
        });
}

std::optional<QByteArray> encodeModePacket(
    const quint32 sequence,
    const protocol::OperatingMode mode)
{
    return encodePayloadPacket<protocol::kModePayloadSize>(
        protocol::MessageType::SetMode,
        sequence,
        [mode](const std::span<protocol::Byte> payload) {
            return protocol::encodeOperatingMode(mode, payload);
        });
}

std::optional<QByteArray> encodeBrightnessPacket(
    const quint32 sequence,
    const protocol::Byte brightness)
{
    return encodePayloadPacket<protocol::kBrightnessPayloadSize>(
        protocol::MessageType::SetBrightness,
        sequence,
        [brightness](const std::span<protocol::Byte> payload) {
            return protocol::encodeBrightness(brightness, payload);
        });
}

std::optional<QByteArray> encodeEffectPacket(
    const quint32 sequence,
    const protocol::EffectConfig& effect)
{
    return encodePayloadPacket<protocol::kEffectConfigPayloadSize>(
        protocol::MessageType::SetEffect,
        sequence,
        [&effect](const std::span<protocol::Byte> payload) {
            return protocol::encodeEffectConfig(effect, payload);
        });
}

std::optional<QByteArray> encodeLedFramePacket(
    const quint32 sequence,
    const QByteArrayView rgbPixels)
{
    if (rgbPixels.isEmpty()
        || (rgbPixels.size() % 3) != 0
        || rgbPixels.size() / 3 > 300) {
        return std::nullopt;
    }

    std::array<
        protocol::Byte,
        protocol::kLedFramePreambleSize + 300U * 3U> payload{};
    std::size_t payloadSize = 0;
    if (!protocol::encodeLedFramePayload(
            static_cast<std::uint16_t>(rgbPixels.size() / 3),
            bytesOf(rgbPixels),
            payload,
            payloadSize)) {
        return std::nullopt;
    }

    return encodeProtocolPacket(
        protocol::MessageType::LedFrame,
        protocol::PacketFlagNone,
        sequence,
        QByteArrayView(
            reinterpret_cast<const char*>(payload.data()),
            static_cast<qsizetype>(payloadSize)));
}

void OutboundPacketQueue::pushControl(QByteArray packet)
{
    if (!packet.isEmpty()) {
        controlPackets_.push_back(std::move(packet));
    }
}

void OutboundPacketQueue::replaceLedFrame(QByteArray packet)
{
    if (packet.isEmpty()) {
        return;
    }
    if (ledFrame_.has_value()) {
        ++replacedFrames_;
    }
    ledFrame_ = std::move(packet);
}

std::optional<QByteArray> OutboundPacketQueue::takeNext()
{
    if (!controlPackets_.empty()) {
        QByteArray result = std::move(controlPackets_.front());
        controlPackets_.pop_front();
        return result;
    }
    if (!ledFrame_.has_value()) {
        return std::nullopt;
    }
    std::optional<QByteArray> result = std::move(ledFrame_);
    ledFrame_.reset();
    return result;
}

qsizetype OutboundPacketQueue::controlPacketCount() const noexcept
{
    return static_cast<qsizetype>(controlPackets_.size());
}

bool OutboundPacketQueue::hasLedFrame() const noexcept
{
    return ledFrame_.has_value();
}

quint64 OutboundPacketQueue::replacedFrameCount() const noexcept
{
    return replacedFrames_;
}

bool OutboundPacketQueue::empty() const noexcept
{
    return controlPackets_.empty() && !ledFrame_.has_value();
}

void OutboundPacketQueue::clearLedFrame()
{
    ledFrame_.reset();
}

void OutboundPacketQueue::clear()
{
    controlPackets_.clear();
    ledFrame_.reset();
}

} // namespace ambilight::transport
