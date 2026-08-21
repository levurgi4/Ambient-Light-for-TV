#pragma once

#include <ambilight/protocol/protocol.hpp>

#include <QByteArray>
#include <QByteArrayView>

#include <deque>
#include <optional>

namespace ambilight::transport {

// Protocol packet construction is intentionally kept free of QObject and
// serial-port state so it can be tested without hardware.
[[nodiscard]] std::optional<QByteArray> encodeProtocolPacket(
    protocol::MessageType type,
    quint16 flags,
    quint32 sequence,
    QByteArrayView payload = {});

[[nodiscard]] std::optional<QByteArray> encodeHelloPacket(quint32 sequence);
[[nodiscard]] std::optional<QByteArray> encodeGetStatePacket(quint32 sequence);
[[nodiscard]] std::optional<QByteArray> encodePingPacket(
    quint32 sequence,
    QByteArrayView payload = {});
[[nodiscard]] std::optional<QByteArray> encodeStripConfigurationPacket(
    quint32 sequence,
    const protocol::StripConfig& configuration);
[[nodiscard]] std::optional<QByteArray> encodeModePacket(
    quint32 sequence,
    protocol::OperatingMode mode);
[[nodiscard]] std::optional<QByteArray> encodeBrightnessPacket(
    quint32 sequence,
    protocol::Byte brightness);
[[nodiscard]] std::optional<QByteArray> encodeEffectPacket(
    quint32 sequence,
    const protocol::EffectConfig& effect);
[[nodiscard]] std::optional<QByteArray> encodeLedFramePacket(
    quint32 sequence,
    QByteArrayView rgbPixels);

// Control packets are reliable and ordered. LED frames are deliberately a
// single replaceable slot: a slow serial link can cost frames, never latency.
class OutboundPacketQueue final {
public:
    void pushControl(QByteArray packet);
    void replaceLedFrame(QByteArray packet);

    [[nodiscard]] std::optional<QByteArray> takeNext();
    [[nodiscard]] qsizetype controlPacketCount() const noexcept;
    [[nodiscard]] bool hasLedFrame() const noexcept;
    [[nodiscard]] quint64 replacedFrameCount() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    void clearLedFrame();
    void clear();

private:
    std::deque<QByteArray> controlPackets_;
    std::optional<QByteArray> ledFrame_;
    quint64 replacedFrames_{};
};

} // namespace ambilight::transport
