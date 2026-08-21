#pragma once

#include "transport_types.hpp"

#include <ambilight/protocol/protocol.hpp>

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QStringView>
#include <QThread>
#include <QVector>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>

namespace ambilight::transport {

class SerialTransportRuntime;

namespace detail {

// Frames are valid only for the exact strip/mapping revision that produced
// them. In particular, accepting a future epoch would let a frame overtake
// the matching SET_STRIP_CONFIG command on callers using multiple threads.
[[nodiscard]] constexpr bool frameEpochMatches(
    const quint64 activeEpoch,
    const quint64 candidateEpoch) noexcept
{
    return activeEpoch == candidateEpoch;
}

[[nodiscard]] constexpr int reconnectTimerIntervalMilliseconds(
    const qint64 now,
    const qint64 nextAttempt) noexcept
{
    constexpr qint64 minimumInterval = 10;
    constexpr qint64 maximumInterval = 1'000;
    const qint64 remaining = nextAttempt - now;
    if (remaining <= minimumInterval) {
        return static_cast<int>(minimumInterval);
    }
    return static_cast<int>(
        remaining < maximumInterval ? remaining : maximumInterval);
}

[[nodiscard]] constexpr bool deadlineExpired(
    const qint64 now,
    const qint64 deadline) noexcept
{
    return deadline > 0 && now >= deadline;
}

[[nodiscard]] constexpr bool handshakeAttemptDue(
    const qint64 now,
    const qint64 nextAttempt,
    const bool controlQueueEmpty,
    const bool writeInProgress) noexcept
{
    return now >= nextAttempt
        && controlQueueEmpty
        && !writeInProgress;
}

enum class StateRequestDeadlineAction {
    Wait,
    Retry,
    Reconnect,
};

[[nodiscard]] constexpr StateRequestDeadlineAction
stateRequestDeadlineAction(
    const qint64 now,
    const qint64 deadline,
    const int attempts,
    const int maximumAttempts) noexcept
{
    if (!deadlineExpired(now, deadline)) {
        return StateRequestDeadlineAction::Wait;
    }
    return attempts < maximumAttempts
        ? StateRequestDeadlineAction::Retry
        : StateRequestDeadlineAction::Reconnect;
}

[[nodiscard]] inline bool hasConservativePortableUsbSerial(
    const QStringView deviceInstanceId,
    const bool reportsUniqueId)
{
    if (!reportsUniqueId) {
        return false;
    }
    const qsizetype separator =
        deviceInstanceId.lastIndexOf(u'\\');
    const QStringView token = separator >= 0
        ? deviceInstanceId.sliced(separator + 1)
        : deviceInstanceId;
    if (token.size() < 4 || token.contains(u'&')) {
        return false;
    }

    QString normalized;
    normalized.reserve(token.size());
    for (const QChar character : token) {
        if (character.isLetterOrNumber()) {
            normalized.append(character.toUpper());
        }
    }
    if (normalized.size() < 4) {
        return false;
    }

    const bool allSame = std::all_of(
        normalized.cbegin() + 1,
        normalized.cend(),
        [first = normalized.front()](const QChar character) {
            return character == first;
        });
    if (allSame
        || normalized == QStringLiteral("0001")
        || normalized == QStringLiteral("1234")
        || normalized == QStringLiteral("123456")
        || normalized == QStringLiteral("12345678")
        || normalized == QStringLiteral("CH340")
        || normalized == QStringLiteral("USB2SERIAL")) {
        return false;
    }
    return true;
}

[[nodiscard]] inline bool trustedPersistentIdentityMatches(
    const QStringView candidateIdentity,
    const QStringView trustedIdentity)
{
    constexpr QStringView prefix = u"setupapi-v1|";
    return !candidateIdentity.isEmpty()
        && candidateIdentity.startsWith(
            prefix,
            Qt::CaseInsensitive)
        && candidateIdentity.compare(
               trustedIdentity,
               Qt::CaseInsensitive)
            == 0;
}

struct PortIdentitySnapshot {
    QString transientFingerprint;
    QString persistentIdentity;
    bool classified{};
    bool automaticEligible{};
    bool bluetooth{};
};

// The transient SetupAPI fingerprint binds a COM alias to the device that was
// enumerated immediately before CreateFile. Automatic mode additionally
// requires the persistent identity previously approved by a manual handshake.
[[nodiscard]] inline bool postOpenPortIdentityMatches(
    const PortSelectionMode selectionMode,
    const PortIdentitySnapshot& expected,
    const PortIdentitySnapshot& observed)
{
    if (!expected.classified
        || !observed.classified
        || expected.bluetooth
        || observed.bluetooth
        || expected.transientFingerprint.isEmpty()
        || expected.transientFingerprint.compare(
               observed.transientFingerprint,
               Qt::CaseInsensitive)
            != 0) {
        return false;
    }
    if (selectionMode == PortSelectionMode::Explicit) {
        return true;
    }
    return expected.automaticEligible
        && observed.automaticEligible
        && trustedPersistentIdentityMatches(
            observed.persistentIdentity,
            expected.persistentIdentity);
}

// Device uptime is a modulo-2^32 counter. A delta in the backwards half of
// the ring identifies a reset, while 0xFFFFFFFF -> 0 remains forward motion.
[[nodiscard]] constexpr bool deviceUptimeIndicatesReset(
    const quint32 previous,
    const quint32 current) noexcept
{
    return current - previous > 0x7FFF'FFFFU;
}

class StateRequestSequenceWindow final {
public:
    static constexpr std::size_t kCapacity = 3;

    [[nodiscard]] bool append(const quint32 sequence) noexcept
    {
        if (size_ >= sequences_.size() || contains(sequence)) {
            return false;
        }
        sequences_[size_++] = sequence;
        return true;
    }

    [[nodiscard]] bool contains(const quint32 sequence) const noexcept
    {
        const auto end = sequences_.cbegin()
            + static_cast<std::ptrdiff_t>(size_);
        return std::find(
                   sequences_.cbegin(),
                   end,
                   sequence)
            != end;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return size_ == 0;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }

    void clear() noexcept
    {
        size_ = 0;
    }

private:
    std::array<quint32, kCapacity> sequences_{};
    std::size_t size_{};
};

// Opening and retiring a broken Windows serial driver may require a detached
// thread so the GUI can still stop. This process-wide budget prevents repeated
// configuration changes from creating an unbounded number of stuck threads.
class DetachedDriverOperationBudget final {
public:
    explicit DetachedDriverOperationBudget(const int maximum) noexcept
        : maximum_(std::max(1, maximum))
    {
    }

    DetachedDriverOperationBudget(
        const DetachedDriverOperationBudget&) = delete;
    DetachedDriverOperationBudget& operator=(
        const DetachedDriverOperationBudget&) = delete;

    [[nodiscard]] bool tryAcquire() noexcept
    {
        if (poisoned_.load(std::memory_order_acquire)) {
            return false;
        }

        int active = active_.load(std::memory_order_relaxed);
        while (active < maximum_) {
            if (active_.compare_exchange_weak(
                    active,
                    active + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                if (!poisoned_.load(std::memory_order_acquire)) {
                    return true;
                }
                release();
                return false;
            }
        }
        return false;
    }

    void release() noexcept
    {
        int active = active_.load(std::memory_order_relaxed);
        while (active > 0
               && !active_.compare_exchange_weak(
                   active,
                   active - 1,
                   std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
    }

    void poison() noexcept
    {
        poisoned_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool isPoisoned() const noexcept
    {
        return poisoned_.load(std::memory_order_acquire);
    }

    [[nodiscard]] int active() const noexcept
    {
        return active_.load(std::memory_order_acquire);
    }

    [[nodiscard]] int maximum() const noexcept
    {
        return maximum_;
    }

private:
    const int maximum_;
    std::atomic_int active_{};
    std::atomic_bool poisoned_{};
};

} // namespace detail

// Thread-safe facade. The COM handle, protocol decoder, timers and outbound
// scheduler live exclusively on the owned transport thread.
class SerialTransportWorker final : public QObject {
    Q_OBJECT

public:
    explicit SerialTransportWorker(QObject* parent = nullptr);
    ~SerialTransportWorker() override;

    SerialTransportWorker(const SerialTransportWorker&) = delete;
    SerialTransportWorker& operator=(const SerialTransportWorker&) = delete;
    SerialTransportWorker(SerialTransportWorker&&) = delete;
    SerialTransportWorker& operator=(SerialTransportWorker&&) = delete;

    [[nodiscard]] static QVector<SerialPortDescriptor> enumeratePorts(
        QString* errorMessage = nullptr);
    // Schedules SetupAPI enumeration on the owned transport thread. Returns
    // false only when the runtime can no longer accept work (for example,
    // during shutdown).
    [[nodiscard]] bool requestPortRefresh();

public slots:
    void start();
    void stop();
    void updateConnectionConfiguration(
        const ambilight::transport::ConnectionConfiguration& configuration);

    void sendStripConfiguration(
        const ambilight::protocol::StripConfig& configuration);
    void sendMode(ambilight::protocol::OperatingMode mode);
    // Programs the mode used by firmware when the Ambilight frame lease
    // expires. Call this before sendMode(Ambilight). Effect/Solid fallback
    // details are supplied with sendEffect().
    void sendFallbackMode(ambilight::protocol::OperatingMode mode);
    void sendBrightness(ambilight::protocol::Byte brightness);
    void sendEffect(const ambilight::protocol::EffectConfig& effect);
    void requestState();
    // Completes asynchronously after all currently desired reliable controls
    // have been written and acknowledged, or after the bounded deadline.
    void requestDrain(int timeoutMilliseconds = 1'500);

    // RGB byte order, exactly three bytes per physical LED. Calls are
    // coalesced before crossing threads; only the most recent pending frame is
    // retained.
    void submitLedFrame(const QByteArray& rgbPixels);
    void clearPendingLedFrame();

signals:
    void portsChanged(
        const QVector<ambilight::transport::SerialPortDescriptor>& ports);
    void portRefreshFinished(
        const QVector<ambilight::transport::SerialPortDescriptor>& ports,
        const QString& errorMessage);
    void statusChanged(
        const ambilight::transport::ConnectionStatus& status);
    void deviceInformationChanged(
        const ambilight::transport::DeviceInformation& information);
    void deviceStateChanged(
        const ambilight::transport::DeviceState& state);
    void commandFinished(
        const ambilight::transport::CommandResult& result);
    void statisticsChanged(
        const ambilight::transport::TransportStatistics& statistics);
    void drainFinished(bool success);

private:
    friend class SerialTransportRuntime;

    struct PendingFrame {
        QByteArray rgbPixels;
        quint64 generation{};
        quint64 frameEpoch{};
    };

    QThread transportThread_;
    SerialTransportRuntime* runtime_ = nullptr;
    std::atomic_bool runningRequested_{false};
    std::atomic<quint64> commandGeneration_{1};
    std::atomic<quint64> frameEpoch_{1};

    QMutex lifecycleMutex_;
    ConnectionConfiguration requestedConnectionConfiguration_;
    QMutex pendingFrameMutex_;
    std::optional<PendingFrame> pendingFrame_;
    bool frameDeliveryScheduled_ = false;
    quint64 frameDeliveryToken_{};
    quint64 facadeReplacedFrames_{};
};

} // namespace ambilight::transport
