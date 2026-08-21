#include "transport/transport_protocol.hpp"
#include "transport/serial_transport_worker.hpp"
#include "transport/transport_types.hpp"

#include <ambilight/protocol/protocol.hpp>

#include <QByteArray>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <optional>
#include <span>

namespace {

using ambilight::protocol::Byte;
using ambilight::protocol::MessageType;

bool expect(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

struct OwnedPacket {
    MessageType type{};
    std::uint16_t flags{};
    std::uint32_t sequence{};
    QByteArray payload;
};

std::optional<OwnedPacket> decodeOne(const QByteArray& wire)
{
    ambilight::protocol::StreamDecoder decoder;
    std::optional<OwnedPacket> result;
    decoder.push(
        std::span<const Byte>(
            reinterpret_cast<const Byte*>(wire.constData()),
            static_cast<std::size_t>(wire.size())),
        [&result](const ambilight::protocol::PacketView& packet) {
            result = OwnedPacket{
                packet.type,
                packet.flags,
                packet.sequence,
                QByteArray(
                    reinterpret_cast<const char*>(packet.payload.data()),
                    static_cast<qsizetype>(packet.payload.size())),
            };
        });
    return result;
}

bool testHandshakeAndStatePackets()
{
    const auto hello =
        ambilight::transport::encodeHelloPacket(17);
    const auto getState =
        ambilight::transport::encodeGetStatePacket(18);
    if (!expect(hello.has_value(), "hello packet encodes")
        || !expect(getState.has_value(), "state packet encodes")) {
        return false;
    }

    const auto decodedHello = decodeOne(*hello);
    const auto decodedState = decodeOne(*getState);
    return expect(decodedHello.has_value(), "hello packet decodes")
        && expect(
            decodedHello->type == MessageType::Hello,
            "hello message type")
        && expect(decodedHello->sequence == 17, "hello sequence")
        && expect(decodedHello->flags == 0, "hello has no flags")
        && expect(
            decodedHello->payload.isEmpty(),
            "hello payload is empty")
        && expect(decodedState.has_value(), "state packet decodes")
        && expect(
            decodedState->type == MessageType::GetState,
            "get-state message type")
        && expect(decodedState->sequence == 18, "get-state sequence");
}

bool testConfigurationAndModePackets()
{
    ambilight::protocol::StripConfig configuration;
    configuration.startOffset = 7;
    configuration.reversed = true;
    configuration.brightnessLimit = 153;
    configuration.redCalibration = 255;
    configuration.greenCalibration = 235;
    configuration.blueCalibration = 200;

    const auto configWire =
        ambilight::transport::encodeStripConfigurationPacket(
            101,
            configuration);
    const auto modeWire = ambilight::transport::encodeModePacket(
        102,
        ambilight::protocol::OperatingMode::Ambilight);
    if (!expect(configWire.has_value(), "strip config encodes")
        || !expect(modeWire.has_value(), "mode encodes")) {
        return false;
    }

    const auto configPacket = decodeOne(*configWire);
    const auto modePacket = decodeOne(*modeWire);
    ambilight::protocol::StripConfig decodedConfiguration;
    ambilight::protocol::OperatingMode decodedMode{};
    return expect(configPacket.has_value(), "strip config decodes")
        && expect(
            configPacket->type == MessageType::SetStripConfig,
            "strip config message type")
        && expect(
            configPacket->flags
                == ambilight::protocol::PacketFlagAckRequested,
            "strip config requests an acknowledgement")
        && expect(
            ambilight::protocol::decodeStripConfig(
                std::span<const Byte>(
                    reinterpret_cast<const Byte*>(
                        configPacket->payload.constData()),
                    static_cast<std::size_t>(
                        configPacket->payload.size())),
                decodedConfiguration),
            "strip config payload decodes")
        && expect(
            decodedConfiguration.startOffset == 7
                && decodedConfiguration.reversed
                && decodedConfiguration.brightnessLimit == 153
                && decodedConfiguration.redCalibration == 255
                && decodedConfiguration.greenCalibration == 235
                && decodedConfiguration.blueCalibration == 200,
            "strip config payload is preserved")
        && expect(modePacket.has_value(), "mode packet decodes")
        && expect(
            ambilight::protocol::decodeOperatingMode(
                std::span<const Byte>(
                    reinterpret_cast<const Byte*>(
                        modePacket->payload.constData()),
                    static_cast<std::size_t>(
                        modePacket->payload.size())),
                decodedMode),
            "mode payload decodes")
        && expect(
            decodedMode
                == ambilight::protocol::OperatingMode::Ambilight,
            "mode payload is preserved");
}

bool testLedFramePacket()
{
    QByteArray pixels;
    pixels.resize(300 * 3);
    for (qsizetype index = 0; index < pixels.size(); ++index) {
        pixels[index] = static_cast<char>(index & 0xFF);
    }

    const auto wire =
        ambilight::transport::encodeLedFramePacket(0xABCDEF01U, pixels);
    if (!expect(wire.has_value(), "maximum LED frame encodes")) {
        return false;
    }
    const auto packet = decodeOne(*wire);
    if (!expect(packet.has_value(), "LED frame packet decodes")) {
        return false;
    }
    const auto frame = ambilight::protocol::decodeLedFramePayload(
        std::span<const Byte>(
            reinterpret_cast<const Byte*>(packet->payload.constData()),
            static_cast<std::size_t>(packet->payload.size())));

    const auto empty =
        ambilight::transport::encodeLedFramePacket(1, {});
    const QByteArray uneven(4, '\0');
    const auto malformed =
        ambilight::transport::encodeLedFramePacket(1, uneven);
    const QByteArray tooLarge(301 * 3, '\0');
    const auto oversized =
        ambilight::transport::encodeLedFramePacket(1, tooLarge);

    return expect(
               packet->type == MessageType::LedFrame,
               "LED frame message type")
        && expect(packet->flags == 0, "LED frame is never ACKed")
        && expect(
            packet->sequence == 0xABCDEF01U,
            "LED frame sequence")
        && expect(frame.valid, "LED frame payload is valid")
        && expect(frame.ledCount == 300, "LED frame count")
        && expect(
            std::equal(
                frame.pixels.begin(),
                frame.pixels.end(),
                reinterpret_cast<const Byte*>(pixels.constData())),
            "LED pixels are preserved")
        && expect(!empty.has_value(), "empty LED frame is rejected")
        && expect(
            !malformed.has_value(),
            "partial RGB pixel is rejected")
        && expect(
            !oversized.has_value(),
            "oversized LED frame is rejected");
}

bool testLatestOnlyQueue()
{
    ambilight::transport::OutboundPacketQueue queue;
    queue.replaceLedFrame(QByteArrayLiteral("frame-1"));
    queue.replaceLedFrame(QByteArrayLiteral("frame-2"));
    queue.pushControl(QByteArrayLiteral("control"));

    const auto first = queue.takeNext();
    const auto second = queue.takeNext();
    bool success =
        expect(first == QByteArrayLiteral("control"), "control wins priority")
        && expect(
            second == QByteArrayLiteral("frame-2"),
            "only the latest LED frame remains")
        && expect(
            queue.replacedFrameCount() == 1,
            "replaced frame is counted")
        && expect(queue.empty(), "queue drains completely");

    queue.replaceLedFrame(QByteArrayLiteral("stale"));
    queue.clearLedFrame();
    success &= expect(
        !queue.hasLedFrame(),
        "pending LED frame can be invalidated");
    return success;
}

bool testConnectionConfiguration()
{
    ambilight::transport::ConnectionConfiguration configuration;
    bool success = expect(
        configuration.isValid(),
        "default connection configuration is valid");

    configuration.selectionMode =
        ambilight::transport::PortSelectionMode::Explicit;
    configuration.portName.clear();
    success &= expect(
        !configuration.isValid(),
        "explicit selection requires a port");

    configuration.portName = QStringLiteral("COM7");
    success &= expect(
        configuration.isValid(),
        "explicit COM port is valid");

    configuration.baudRate = 115'200;
    success &= expect(
        !configuration.isValid(),
        "firmware baud rate is fixed at 2 Mbaud");
    configuration.baudRate = 2'000'000;

    configuration.frameKeepAliveIntervalMilliseconds = 24;
    success &= expect(
        !configuration.isValid(),
        "too-fast frame keepalive is rejected");
    configuration.frameKeepAliveIntervalMilliseconds = 250;
    configuration.connectionTimeoutMilliseconds = 500;
    success &= expect(
        !configuration.isValid(),
        "connection timeout cannot predate handshake");
    configuration.connectionTimeoutMilliseconds = 4'000;
    configuration.statePollIntervalMilliseconds = 2'001;
    success &= expect(
        !configuration.isValid(),
        "state polling must run at least twice per connection timeout");
    return success;
}

bool testTransportSchedulingPolicy()
{
    using ambilight::transport::detail::deadlineExpired;
    using ambilight::transport::detail::frameEpochMatches;
    using ambilight::transport::detail::
        hasConservativePortableUsbSerial;
    using ambilight::transport::detail::handshakeAttemptDue;
    using ambilight::transport::detail::
        reconnectTimerIntervalMilliseconds;
    using ambilight::transport::detail::StateRequestDeadlineAction;
    using ambilight::transport::detail::
        stateRequestDeadlineAction;
    using ambilight::transport::detail::
        trustedPersistentIdentityMatches;

    const QString trustedIdentity = QStringLiteral(
        "setupapi-v1|VID_10C4&PID_EA60|"
        "USB\\VID_10C4&PID_EA60\\A1B2C3D4|"
        "00112233-4455-6677-8899-AABBCCDDEEFF");

    return expect(
               frameEpochMatches(7, 7),
               "matching frame epoch is accepted")
        && expect(
            !frameEpochMatches(7, 6),
            "stale frame epoch is rejected")
        && expect(
            !frameEpochMatches(7, 8),
            "future frame epoch is rejected")
        && expect(
            reconnectTimerIntervalMilliseconds(1'000, 1'005) == 10,
            "near reconnect deadline does not busy-poll")
        && expect(
            reconnectTimerIntervalMilliseconds(1'000, 1'250) == 250,
            "reconnect timer follows a nearby deadline")
        && expect(
            reconnectTimerIntervalMilliseconds(1'000, 5'000) == 1'000,
            "long reconnect waits use a bounded coarse tick")
        && expect(
            !deadlineExpired(999, 1'000),
            "deadline remains pending before its due time")
        && expect(
            deadlineExpired(1'000, 1'000),
            "deadline expires at its due time")
        && expect(
            !handshakeAttemptDue(299, 300, true, false),
            "handshake respects the device settle delay")
        && expect(
            handshakeAttemptDue(300, 300, true, false),
            "handshake starts at the settle deadline")
        && expect(
            !handshakeAttemptDue(500, 300, false, false),
            "handshake retry does not accumulate in the control queue")
        && expect(
            stateRequestDeadlineAction(749, 750, 1, 3)
                == StateRequestDeadlineAction::Wait,
            "state request waits for its response deadline")
        && expect(
            stateRequestDeadlineAction(750, 750, 1, 3)
                == StateRequestDeadlineAction::Retry,
            "state request retries at its response deadline")
        && expect(
            stateRequestDeadlineAction(2'250, 2'250, 3, 3)
                == StateRequestDeadlineAction::Reconnect,
            "state request retry budget is bounded")
        && expect(
            hasConservativePortableUsbSerial(
                QStringLiteral(
                    "USB\\VID_10C4&PID_EA60\\A1B2C3D4"),
                true),
            "unique USB serial is eligible for automatic mode")
        && expect(
            !hasConservativePortableUsbSerial(
                QStringLiteral(
                    "USB\\VID_1A86&PID_7523\\5&2A11C0&0&3"),
                false),
            "topology-derived clone identity is manual-only")
        && expect(
            !hasConservativePortableUsbSerial(
                QStringLiteral(
                    "USB\\VID_10C4&PID_EA60\\0001"),
                true),
            "known placeholder serial is manual-only")
        && expect(
            trustedPersistentIdentityMatches(
                trustedIdentity.toLower(),
                trustedIdentity),
            "persistent identity comparison is case insensitive")
        && expect(
            !trustedPersistentIdentityMatches(
                QStringLiteral("\\Device\\USBSER000"),
                trustedIdentity),
            "legacy QueryDosDevice target is never trusted")
        && expect(
            !trustedPersistentIdentityMatches(
                trustedIdentity
                    + QStringLiteral("-DIFFERENT-CONTAINER"),
                trustedIdentity),
            "different persistent device identity is rejected");
}

bool testRecoveryIdentityAndDriverBudgetPolicies()
{
    using ambilight::transport::PortSelectionMode;
    using ambilight::transport::detail::
        DetachedDriverOperationBudget;
    using ambilight::transport::detail::
        deviceUptimeIndicatesReset;
    using ambilight::transport::detail::
        PortIdentitySnapshot;
    using ambilight::transport::detail::
        postOpenPortIdentityMatches;
    using ambilight::transport::detail::
        StateRequestSequenceWindow;

    bool success = expect(
        !deviceUptimeIndicatesReset(1'000, 1'001),
        "increasing controller uptime is normal");
    success &= expect(
        !deviceUptimeIndicatesReset(1'000, 1'000),
        "equal controller uptime is normal");
    success &= expect(
        deviceUptimeIndicatesReset(5'000, 25),
        "short backwards uptime jump identifies a controller reset");
    success &= expect(
        !deviceUptimeIndicatesReset(0xFFFF'FFF0U, 0x20U),
        "natural uint32 uptime wrap is forward motion");

    StateRequestSequenceWindow stateRequests;
    success &= expect(
        stateRequests.append(100)
            && stateRequests.append(101)
            && stateRequests.contains(100)
            && stateRequests.contains(101),
        "all state retry sequences remain correlated");
    success &= expect(
        !stateRequests.contains(999),
        "unsolicited state sequence is not correlated");
    success &= expect(
        !stateRequests.append(100),
        "duplicate state retry sequence is rejected");
    success &= expect(
        stateRequests.append(102)
            && !stateRequests.append(103),
        "state retry correlation window is bounded");
    stateRequests.clear();
    success &= expect(
        stateRequests.empty(),
        "completed state request clears every retry sequence");

    const QString persistentIdentity = QStringLiteral(
        "setupapi-v1|VID_10C4&PID_EA60|"
        "USB\\VID_10C4&PID_EA60\\A1B2C3D4|"
        "00112233-4455-6677-8899-AABBCCDDEEFF");
    const PortIdentitySnapshot expected{
        QStringLiteral(
            "setupapi-open-v1|USB\\VID_10C4&PID_EA60\\A1B2C3D4|"
            "00112233-4455-6677-8899-AABBCCDDEEFF"),
        persistentIdentity,
        true,
        true,
        false,
    };
    PortIdentitySnapshot observed = expected;
    success &= expect(
        postOpenPortIdentityMatches(
            PortSelectionMode::Automatic,
            expected,
            observed),
        "post-open automatic identity accepts the same devnode");
    observed.transientFingerprint += QStringLiteral("-REPLACED");
    success &= expect(
        !postOpenPortIdentityMatches(
            PortSelectionMode::Automatic,
            expected,
            observed),
        "post-open identity rejects a COM alias reassignment");
    const PortIdentitySnapshot manualExpected{
        expected.transientFingerprint,
        {},
        true,
        false,
        false,
    };
    observed = manualExpected;
    observed.bluetooth = true;
    success &= expect(
        !postOpenPortIdentityMatches(
            PortSelectionMode::Explicit,
            manualExpected,
            observed),
        "post-open identity rejects Bluetooth in manual mode");
    observed = expected;
    observed.persistentIdentity += QStringLiteral("-OTHER");
    success &= expect(
        !postOpenPortIdentityMatches(
            PortSelectionMode::Automatic,
            expected,
            observed),
        "automatic mode revalidates the persistent identity");
    success &= expect(
        postOpenPortIdentityMatches(
            PortSelectionMode::Explicit,
            manualExpected,
            manualExpected),
        "manual mode requires the same transient devnode only");

    DetachedDriverOperationBudget budget(2);
    success &= expect(
        budget.tryAcquire() && budget.tryAcquire(),
        "detached driver operation budget grants bounded slots");
    success &= expect(
        !budget.tryAcquire() && budget.active() == 2,
        "detached driver operation budget refuses overflow");
    budget.release();
    success &= expect(
        budget.tryAcquire() && budget.active() == 2,
        "completed driver operation releases its slot");
    budget.poison();
    budget.release();
    budget.release();
    success &= expect(
        budget.active() == 0
            && !budget.tryAcquire(),
        "poisoned driver budget prevents unsafe future opens");

    return success;
}

} // namespace

int main()
{
    const bool success = testHandshakeAndStatePackets()
        && testConfigurationAndModePackets()
        && testLedFramePacket()
        && testLatestOnlyQueue()
        && testConnectionConfiguration()
        && testTransportSchedulingPolicy()
        && testRecoveryIdentityAndDriverBudgetPolicies();
    if (success) {
        std::cout << "All serial transport tests passed.\n";
    }
    return success ? 0 : 1;
}
