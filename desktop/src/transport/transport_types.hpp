#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

namespace ambilight::transport {

enum class PortSelectionMode {
    Automatic,
    Explicit,
};

enum class ConnectionState {
    Stopped,
    Searching,
    Opening,
    Handshaking,
    Connected,
    Reconnecting,
    Error,
};

struct SerialPortDescriptor {
    QString portName;
    QString systemLocation;
    QString description;

    [[nodiscard]] bool operator==(
        const SerialPortDescriptor&) const noexcept = default;
};

struct ConnectionConfiguration {
    PortSelectionMode selectionMode{PortSelectionMode::Automatic};
    QString portName;
    int baudRate{2'000'000};
    int reconnectIntervalMilliseconds{1'000};
    int handshakeTimeoutMilliseconds{900};
    int statePollIntervalMilliseconds{1'000};
    int connectionTimeoutMilliseconds{4'000};
    // A static desktop produces no new DXGI frame. Re-sending the retained
    // RGB frame keeps the ESP32 ambient lease alive.
    int frameKeepAliveIntervalMilliseconds{250};

    [[nodiscard]] bool isValid() const noexcept
    {
        return (selectionMode == PortSelectionMode::Automatic
                || !portName.trimmed().isEmpty())
            // The current ESP32 firmware owns UART0 at a fixed 2 Mbaud.
            && baudRate == 2'000'000
            && reconnectIntervalMilliseconds >= 100
            && reconnectIntervalMilliseconds <= 60'000
            && handshakeTimeoutMilliseconds >= 100
            && handshakeTimeoutMilliseconds <= 10'000
            && statePollIntervalMilliseconds >= 100
            && statePollIntervalMilliseconds <= 60'000
            && connectionTimeoutMilliseconds
                >= handshakeTimeoutMilliseconds
            && connectionTimeoutMilliseconds <= 120'000
            // Leave room for at least one unanswered poll before declaring
            // the connection stale.
            && statePollIntervalMilliseconds
                <= connectionTimeoutMilliseconds / 2
            && frameKeepAliveIntervalMilliseconds >= 25
            && frameKeepAliveIntervalMilliseconds <= 5'000;
    }

    [[nodiscard]] bool operator==(
        const ConnectionConfiguration&) const noexcept = default;
};

struct DeviceInformation {
    int firmwareMajor{};
    int firmwareMinor{};
    int firmwarePatch{};
    int protocolVersion{};
    int maximumLedCount{};
    int configuredLedCount{};
    quint32 capabilities{};
    quint32 uptimeMilliseconds{};

    [[nodiscard]] bool operator==(
        const DeviceInformation&) const noexcept = default;
};

struct DeviceState {
    int mode{};
    int effect{};
    int brightness{};
    quint8 flags{};
    int ledCount{};
    int currentLimitMilliamps{};
    quint32 lastReceivedSequence{};
    quint32 lastAppliedSequence{};
    quint32 crcErrors{};
    quint32 droppedFrames{};
    quint32 uptimeMilliseconds{};

    [[nodiscard]] bool operator==(
        const DeviceState&) const noexcept = default;
};

struct ConnectionStatus {
    ConnectionState state{ConnectionState::Stopped};
    QString portName;
    QString message;
    quint32 nativeError{};
    quint64 reconnectAttempt{};

    [[nodiscard]] bool operator==(
        const ConnectionStatus&) const noexcept = default;
};

struct CommandResult {
    int messageType{};
    quint32 sequence{};
    bool success{};
    int errorCode{};
    int detail{};
    QString message;

    [[nodiscard]] bool operator==(
        const CommandResult&) const noexcept = default;
};

struct TransportStatistics {
    quint64 transmittedBytes{};
    quint64 receivedBytes{};
    quint64 transmittedFrames{};
    quint64 replacedFrames{};
    quint64 protocolErrors{};

    [[nodiscard]] bool operator==(
        const TransportStatistics&) const noexcept = default;
};

} // namespace ambilight::transport

Q_DECLARE_METATYPE(ambilight::transport::PortSelectionMode)
Q_DECLARE_METATYPE(ambilight::transport::ConnectionState)
Q_DECLARE_METATYPE(ambilight::transport::SerialPortDescriptor)
Q_DECLARE_METATYPE(QVector<ambilight::transport::SerialPortDescriptor>)
Q_DECLARE_METATYPE(ambilight::transport::ConnectionConfiguration)
Q_DECLARE_METATYPE(ambilight::transport::DeviceInformation)
Q_DECLARE_METATYPE(ambilight::transport::DeviceState)
Q_DECLARE_METATYPE(ambilight::transport::ConnectionStatus)
Q_DECLARE_METATYPE(ambilight::transport::CommandResult)
Q_DECLARE_METATYPE(ambilight::transport::TransportStatistics)
