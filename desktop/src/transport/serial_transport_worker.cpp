#include "serial_transport_worker.hpp"

#include "transport_protocol.hpp"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QMutexLocker>
#include <QPointer>
#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <SetupAPI.h>

namespace ambilight::transport {

namespace {

constexpr int kIoTickMilliseconds = 2;
constexpr int kOpenPollMilliseconds = 20;
constexpr int kPortOpenTimeoutMilliseconds = 1'500;
constexpr int kHandshakeSettleMilliseconds = 300;
constexpr int kHandshakeRetryMilliseconds = 200;
constexpr int kAckTimeoutMilliseconds = 1'500;
constexpr int kStateRequestTimeoutMilliseconds = 750;
constexpr int kStatisticsIntervalMilliseconds = 1'000;
constexpr int kMaximumDetachedDriverOperations = 4;
constexpr std::size_t kReadBufferSize = 2'048;
constexpr auto kTrustedPortNameKey =
    "transport/trustedControllerPort";
constexpr auto kTrustedPortIdentityKey =
    "transport/trustedControllerPersistentIdentityV1";
constexpr auto kLegacyTrustedPortIdentityKey =
    "transport/trustedControllerIdentity";

[[nodiscard]] bool isComPortName(const QString& name)
{
    if (!name.startsWith(QStringLiteral("COM"), Qt::CaseInsensitive)
        || name.size() <= 3) {
        return false;
    }
    for (qsizetype index = 3; index < name.size(); ++index) {
        if (!name[index].isDigit()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] int comPortNumber(const QString& name)
{
    bool ok = false;
    const int number = name.sliced(3).toInt(&ok);
    return ok ? number : std::numeric_limits<int>::max();
}

struct NativePortRecord {
    SerialPortDescriptor descriptor;
    detail::PortIdentitySnapshot identity;
};

constexpr GUID kPortsClassGuid{
    0x4d36e978,
    0xe325,
    0x11ce,
    {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18},
};

constexpr DEVPROPKEY kContainerIdProperty{
    {
        0x8c7ed206,
        0x3f8a,
        0x4827,
        {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c},
    },
    2,
};

class DeviceInformationSet final {
public:
    explicit DeviceInformationSet(const HDEVINFO handle) noexcept
        : handle_(handle)
    {
    }

    ~DeviceInformationSet()
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(handle_);
        }
    }

    DeviceInformationSet(const DeviceInformationSet&) = delete;
    DeviceInformationSet& operator=(const DeviceInformationSet&) = delete;

private:
    HDEVINFO handle_ = INVALID_HANDLE_VALUE;
};

[[nodiscard]] QString deviceRegistryString(
    const HDEVINFO devices,
    SP_DEVINFO_DATA& device,
    const DWORD property)
{
    DWORD type = 0;
    DWORD required = 0;
    SetupDiGetDeviceRegistryPropertyW(
        devices,
        &device,
        property,
        &type,
        nullptr,
        0,
        &required);
    if (required < sizeof(wchar_t)
        || (type != REG_SZ
            && type != REG_EXPAND_SZ
            && type != REG_MULTI_SZ)) {
        return {};
    }

    const std::size_t characterCapacity =
        static_cast<std::size_t>(
            (required + sizeof(wchar_t) - 1)
            / sizeof(wchar_t))
        + 1;
    std::vector<wchar_t> value(
        characterCapacity,
        L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(
            devices,
            &device,
            property,
            &type,
            reinterpret_cast<PBYTE>(value.data()),
            static_cast<DWORD>(
                value.size() * sizeof(wchar_t)),
            nullptr)) {
        return {};
    }
    return QString::fromWCharArray(value.data());
}

[[nodiscard]] QString deviceInstanceId(
    const HDEVINFO devices,
    SP_DEVINFO_DATA& device)
{
    DWORD required = 0;
    SetupDiGetDeviceInstanceIdW(
        devices,
        &device,
        nullptr,
        0,
        &required);
    if (required <= 1) {
        return {};
    }
    std::vector<wchar_t> value(required, L'\0');
    if (!SetupDiGetDeviceInstanceIdW(
            devices,
            &device,
            value.data(),
            required,
            nullptr)) {
        return {};
    }
    return QString::fromWCharArray(value.data()).toUpper();
}

[[nodiscard]] QString devicePortName(
    const HDEVINFO devices,
    SP_DEVINFO_DATA& device)
{
    const HKEY key = SetupDiOpenDevRegKey(
        devices,
        &device,
        DICS_FLAG_GLOBAL,
        0,
        DIREG_DEV,
        KEY_QUERY_VALUE);
    if (key == INVALID_HANDLE_VALUE) {
        return {};
    }

    DWORD type = 0;
    DWORD required = 0;
    const LSTATUS sizeStatus = RegQueryValueExW(
        key,
        L"PortName",
        nullptr,
        &type,
        nullptr,
        &required);
    if (sizeStatus != ERROR_SUCCESS
        || (type != REG_SZ && type != REG_EXPAND_SZ)
        || required < sizeof(wchar_t)) {
        RegCloseKey(key);
        return {};
    }

    const std::size_t characterCapacity =
        static_cast<std::size_t>(
            (required + sizeof(wchar_t) - 1)
            / sizeof(wchar_t))
        + 1;
    std::vector<wchar_t> value(
        characterCapacity,
        L'\0');
    DWORD capacityBytes = static_cast<DWORD>(
        value.size() * sizeof(wchar_t));
    const LSTATUS valueStatus = RegQueryValueExW(
        key,
        L"PortName",
        nullptr,
        &type,
        reinterpret_cast<LPBYTE>(value.data()),
        &capacityBytes);
    RegCloseKey(key);
    if (valueStatus != ERROR_SUCCESS) {
        return {};
    }
    return QString::fromWCharArray(value.data())
        .trimmed()
        .toUpper();
}

[[nodiscard]] DWORD deviceCapabilities(
    const HDEVINFO devices,
    SP_DEVINFO_DATA& device)
{
    DWORD capabilities = 0;
    DWORD type = 0;
    DWORD required = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(
            devices,
            &device,
            SPDRP_CAPABILITIES,
            &type,
            reinterpret_cast<PBYTE>(&capabilities),
            sizeof(capabilities),
            &required)
        || type != REG_DWORD) {
        return 0;
    }
    return capabilities;
}

[[nodiscard]] std::optional<GUID> deviceContainerId(
    const HDEVINFO devices,
    SP_DEVINFO_DATA& device)
{
    GUID container{};
    constexpr GUID emptyContainer{};
    DEVPROPTYPE type = 0;
    DWORD required = 0;
    if (!SetupDiGetDevicePropertyW(
            devices,
            &device,
            &kContainerIdProperty,
            &type,
            reinterpret_cast<PBYTE>(&container),
            sizeof(container),
            &required,
            0)
        || type != DEVPROP_TYPE_GUID
        || required != sizeof(container)
        || IsEqualGUID(container, emptyContainer)) {
        return std::nullopt;
    }
    return container;
}

[[nodiscard]] QString guidText(const GUID& value)
{
    return QStringLiteral(
               "%1-%2-%3-%4%5-%6%7%8%9%10%11")
        .arg(value.Data1, 8, 16, QChar(u'0'))
        .arg(value.Data2, 4, 16, QChar(u'0'))
        .arg(value.Data3, 4, 16, QChar(u'0'))
        .arg(value.Data4[0], 2, 16, QChar(u'0'))
        .arg(value.Data4[1], 2, 16, QChar(u'0'))
        .arg(value.Data4[2], 2, 16, QChar(u'0'))
        .arg(value.Data4[3], 2, 16, QChar(u'0'))
        .arg(value.Data4[4], 2, 16, QChar(u'0'))
        .arg(value.Data4[5], 2, 16, QChar(u'0'))
        .arg(value.Data4[6], 2, 16, QChar(u'0'))
        .arg(value.Data4[7], 2, 16, QChar(u'0'))
        .toUpper();
}

[[nodiscard]] QString transientPortFingerprint(
    const QString& instance,
    const std::optional<GUID>& container)
{
    if (instance.isEmpty()) {
        return {};
    }
    return QStringLiteral("setupapi-open-v1|%1|%2")
        .arg(
            instance,
            container.has_value()
                ? guidText(*container)
                : QStringLiteral("NO-CONTAINER"));
}

[[nodiscard]] QString usbVidPid(const QString& hardwareId)
{
    const QString upper = hardwareId.toUpper();
    const qsizetype vid = upper.indexOf(QStringLiteral("VID_"));
    const qsizetype pid = upper.indexOf(QStringLiteral("PID_"));
    if (vid < 0 || pid < 0
        || vid + 8 > upper.size()
        || pid + 8 > upper.size()) {
        return {};
    }
    const QString vidValue = upper.sliced(vid + 4, 4);
    const QString pidValue = upper.sliced(pid + 4, 4);
    const auto hexadecimal = [](const QString& value) {
        return std::all_of(
            value.cbegin(),
            value.cend(),
            [](const QChar character) {
                const ushort code = character.unicode();
                return (code >= u'0' && code <= u'9')
                    || (code >= u'A' && code <= u'F');
            });
    };
    if (!hexadecimal(vidValue) || !hexadecimal(pidValue)) {
        return {};
    }
    return QStringLiteral("VID_%1&PID_%2")
        .arg(vidValue, pidValue);
}

[[nodiscard]] QVector<NativePortRecord> enumerateDosPorts(
    QString* errorMessage)
{
    DWORD capacity = 32'768;
    std::vector<wchar_t> names(capacity);
    DWORD characters = 0;
    for (;;) {
        characters = QueryDosDeviceW(nullptr, names.data(), capacity);
        if (characters != 0) {
            break;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER || capacity >= 1'048'576) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral(
                    "QueryDosDevice failed (%1).").arg(error);
            }
            return {};
        }
        capacity *= 2;
        names.resize(capacity);
    }

    QVector<NativePortRecord> ports;
    const wchar_t* cursor = names.data();
    const wchar_t* const end = names.data() + characters;
    while (cursor < end && *cursor != L'\0') {
        const QString name = QString::fromWCharArray(cursor);
        cursor += name.size() + 1;
        if (!isComPortName(name)) {
            continue;
        }

        std::array<wchar_t, 2'048> target{};
        const DWORD targetLength = QueryDosDeviceW(
            reinterpret_cast<LPCWSTR>(name.utf16()),
            target.data(),
            static_cast<DWORD>(target.size()));
        const QString targetName =
            targetLength == 0
            ? QString()
            : QString::fromWCharArray(target.data());
        ports.push_back({
            {
                name.toUpper(),
                QStringLiteral("\\\\.\\%1").arg(name.toUpper()),
                targetName,
            },
            {
                {},
                {},
                targetLength != 0,
                false,
                targetName.contains(
                    QStringLiteral("BthModem"),
                    Qt::CaseInsensitive),
            },
        });
    }
    return ports;
}

void enrichPortsWithSetupApi(
    QVector<NativePortRecord>& ports,
    QString* errorMessage)
{
    const HDEVINFO rawDevices = SetupDiGetClassDevsW(
        &kPortsClassGuid,
        nullptr,
        nullptr,
        DIGCF_PRESENT);
    if (rawDevices == INVALID_HANDLE_VALUE) {
        if (errorMessage != nullptr && errorMessage->isEmpty()) {
            *errorMessage = QStringLiteral(
                "SetupAPI port enumeration failed (%1); automatic "
                "connection is unavailable.")
                .arg(GetLastError());
        }
        return;
    }
    const DeviceInformationSet devices(rawDevices);

    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(rawDevices, index, &device)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS
                && errorMessage != nullptr
                && errorMessage->isEmpty()) {
                *errorMessage = QStringLiteral(
                    "SetupAPI device enumeration stopped (%1).")
                    .arg(GetLastError());
            }
            break;
        }

        const QString portName =
            devicePortName(rawDevices, device);
        if (!isComPortName(portName)) {
            continue;
        }
        auto found = std::find_if(
            ports.begin(),
            ports.end(),
            [&portName](const NativePortRecord& port) {
                return port.descriptor.portName.compare(
                           portName,
                           Qt::CaseInsensitive)
                    == 0;
            });
        if (found == ports.end()) {
            ports.push_back({
                {
                    portName,
                    QStringLiteral("\\\\.\\%1").arg(portName),
                    {},
                },
            });
            found = std::prev(ports.end());
        }

        const QString friendlyName = deviceRegistryString(
            rawDevices,
            device,
            SPDRP_FRIENDLYNAME);
        if (!friendlyName.isEmpty()) {
            found->descriptor.description = friendlyName;
        }
        found->identity.classified = true;

        const QString instance =
            deviceInstanceId(rawDevices, device);
        const QString hardwareId = deviceRegistryString(
            rawDevices,
            device,
            SPDRP_HARDWAREID);
        const QString enumerator = deviceRegistryString(
            rawDevices,
            device,
            SPDRP_ENUMERATOR_NAME);
        const QString service = deviceRegistryString(
            rawDevices,
            device,
            SPDRP_SERVICE);
        found->identity.bluetooth = found->identity.bluetooth
            || instance.startsWith(
                QStringLiteral("BTH"),
                Qt::CaseInsensitive)
            || enumerator.contains(
                QStringLiteral("BTH"),
                Qt::CaseInsensitive)
            || service.contains(
                QStringLiteral("BTH"),
                Qt::CaseInsensitive);

        const DWORD capabilities =
            deviceCapabilities(rawDevices, device);
        const bool portableSerial =
            detail::hasConservativePortableUsbSerial(
                instance,
                (capabilities & CM_DEVCAP_UNIQUEID) != 0);
        const QString vidPid = usbVidPid(hardwareId);
        const std::optional<GUID> container =
            deviceContainerId(rawDevices, device);
        found->identity.transientFingerprint =
            transientPortFingerprint(instance, container);
        if (portableSerial
            && !vidPid.isEmpty()
            && container.has_value()) {
            found->identity.persistentIdentity =
                QStringLiteral("setupapi-v1|%1|%2|%3")
                    .arg(
                        vidPid,
                        instance,
                        guidText(*container));
            found->identity.automaticEligible = true;
        }
    }
}

[[nodiscard]] QVector<NativePortRecord> enumerateNativePortRecords(
    QString* errorMessage)
{
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    QVector<NativePortRecord> ports =
        enumerateDosPorts(errorMessage);
    enrichPortsWithSetupApi(ports, errorMessage);

    std::sort(
        ports.begin(),
        ports.end(),
        [](const NativePortRecord& first,
           const NativePortRecord& second) {
            const int firstNumber =
                comPortNumber(first.descriptor.portName);
            const int secondNumber =
                comPortNumber(second.descriptor.portName);
            if (firstNumber != secondNumber) {
                return firstNumber < secondNumber;
            }
            return first.descriptor.portName
                < second.descriptor.portName;
        });
    return ports;
}

[[nodiscard]] QVector<SerialPortDescriptor> enumerateNativePorts(
    QString* errorMessage)
{
    const QVector<NativePortRecord> records =
        enumerateNativePortRecords(errorMessage);
    QVector<SerialPortDescriptor> result;
    result.reserve(records.size());
    for (const NativePortRecord& record : records) {
        result.push_back(record.descriptor);
    }
    return result;
}

class NativeSerialPort final {
public:
    NativeSerialPort() = default;
    ~NativeSerialPort()
    {
        close();
    }

    NativeSerialPort(const NativeSerialPort&) = delete;
    NativeSerialPort& operator=(const NativeSerialPort&) = delete;

    [[nodiscard]] bool open(
        const QString& portName,
        const int baudRate,
        DWORD& error)
    {
        close();
        error = ERROR_SUCCESS;

        readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (readEvent_ == nullptr || writeEvent_ == nullptr) {
            error = GetLastError();
            close();
            return false;
        }
        readOverlapped_.hEvent = readEvent_;
        writeOverlapped_.hEvent = writeEvent_;

        QString normalizedName = portName.trimmed().toUpper();
        const QString systemLocation =
            normalizedName.startsWith(QStringLiteral("\\\\.\\"))
            ? normalizedName
            : QStringLiteral("\\\\.\\%1").arg(normalizedName);
        handle_ = CreateFileW(
            reinterpret_cast<LPCWSTR>(systemLocation.utf16()),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            close();
            return false;
        }

        if (!SetupComm(handle_, 8'192, 4'096)) {
            error = GetLastError();
            close();
            return false;
        }

        DCB state{};
        state.DCBlength = sizeof(state);
        if (!GetCommState(handle_, &state)) {
            error = GetLastError();
            close();
            return false;
        }
        state.BaudRate = static_cast<DWORD>(baudRate);
        state.ByteSize = 8;
        state.Parity = NOPARITY;
        state.StopBits = ONESTOPBIT;
        state.fBinary = TRUE;
        state.fParity = FALSE;
        state.fOutxCtsFlow = FALSE;
        state.fOutxDsrFlow = FALSE;
        state.fDtrControl = DTR_CONTROL_DISABLE;
        state.fDsrSensitivity = FALSE;
        state.fTXContinueOnXoff = TRUE;
        state.fOutX = FALSE;
        state.fInX = FALSE;
        state.fErrorChar = FALSE;
        state.fNull = FALSE;
        state.fRtsControl = RTS_CONTROL_DISABLE;
        state.fAbortOnError = FALSE;
        if (!SetCommState(handle_, &state)) {
            error = GetLastError();
            close();
            return false;
        }

        COMMTIMEOUTS timeouts{};
        // This documented MAXDWORD/0/0 combination makes reads return
        // immediately with the bytes already buffered by the driver. With
        // all-zero read timeouts Windows may wait for the full 2048-byte
        // request, which would stall the much smaller handshake response.
        timeouts.ReadIntervalTimeout = MAXDWORD;
        if (!SetCommTimeouts(handle_, &timeouts)) {
            error = GetLastError();
            close();
            return false;
        }

        // Clearing stale buffers does not assert DTR/RTS. Those control lines
        // remain disabled for the complete lifetime of this handle.
        if (!PurgeComm(
                handle_,
                PURGE_RXABORT | PURGE_RXCLEAR
                    | PURGE_TXABORT | PURGE_TXCLEAR)) {
            error = GetLastError();
            close();
            return false;
        }

        return true;
    }

    void close() noexcept
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            // CancelIoEx only requests cancellation. The OVERLAPPED structs
            // and their buffers must stay alive until each request reaches a
            // terminal state, otherwise an unplug/reconnect can let an old
            // driver completion overwrite a reused request.
            CancelIoEx(handle_, nullptr);
            settlePendingOperation(readOverlapped_, readPending_);
            settlePendingOperation(writeOverlapped_, writePending_);
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        if (readEvent_ != nullptr) {
            CloseHandle(readEvent_);
            readEvent_ = nullptr;
        }
        if (writeEvent_ != nullptr) {
            CloseHandle(writeEvent_);
            writeEvent_ = nullptr;
        }
        readPending_ = false;
        writePending_ = false;
        writeBuffer_.clear();
        writeOffset_ = 0;
        readOverlapped_ = {};
        writeOverlapped_ = {};
    }

    [[nodiscard]] bool isOpen() const noexcept
    {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] bool hasWriteInProgress() const noexcept
    {
        return writePending_ || !writeBuffer_.isEmpty();
    }

    [[nodiscard]] bool beginWrite(QByteArray bytes)
    {
        if (!isOpen() || hasWriteInProgress() || bytes.isEmpty()) {
            return false;
        }
        writeBuffer_ = std::move(bytes);
        writeOffset_ = 0;
        return true;
    }

    [[nodiscard]] bool serviceRead(
        QByteArray& received,
        DWORD& error)
    {
        error = ERROR_SUCCESS;
        if (!isOpen()) {
            error = ERROR_INVALID_HANDLE;
            return false;
        }

        for (int iteration = 0; iteration < 8; ++iteration) {
            if (readPending_) {
                DWORD transferred = 0;
                if (!GetOverlappedResult(
                        handle_,
                        &readOverlapped_,
                        &transferred,
                        FALSE)) {
                    error = GetLastError();
                    if (error == ERROR_IO_INCOMPLETE) {
                        error = ERROR_SUCCESS;
                        return true;
                    }
                    return false;
                }
                readPending_ = false;
                if (transferred != 0) {
                    received.append(
                        reinterpret_cast<const char*>(readBuffer_.data()),
                        static_cast<qsizetype>(transferred));
                }
            }

            ResetEvent(readEvent_);
            readOverlapped_.Offset = 0;
            readOverlapped_.OffsetHigh = 0;
            DWORD transferred = 0;
            if (ReadFile(
                    handle_,
                    readBuffer_.data(),
                    static_cast<DWORD>(readBuffer_.size()),
                    &transferred,
                    &readOverlapped_)) {
                if (transferred == 0) {
                    return true;
                }
                received.append(
                    reinterpret_cast<const char*>(readBuffer_.data()),
                    static_cast<qsizetype>(transferred));
                continue;
            }

            error = GetLastError();
            if (error == ERROR_IO_PENDING) {
                readPending_ = true;
                error = ERROR_SUCCESS;
                return true;
            }
            return false;
        }
        return true;
    }

    [[nodiscard]] bool serviceWrite(
        bool& completed,
        qsizetype& completedBytes,
        DWORD& error)
    {
        completed = false;
        completedBytes = 0;
        error = ERROR_SUCCESS;
        if (!isOpen()) {
            error = ERROR_INVALID_HANDLE;
            return false;
        }
        if (!hasWriteInProgress()) {
            return true;
        }

        for (int iteration = 0; iteration < 8; ++iteration) {
            if (writePending_) {
                DWORD transferred = 0;
                if (!GetOverlappedResult(
                        handle_,
                        &writeOverlapped_,
                        &transferred,
                        FALSE)) {
                    error = GetLastError();
                    if (error == ERROR_IO_INCOMPLETE) {
                        error = ERROR_SUCCESS;
                        return true;
                    }
                    return false;
                }
                writePending_ = false;
                writeOffset_ += static_cast<qsizetype>(transferred);
            }

            if (writeOffset_ >= writeBuffer_.size()) {
                completed = true;
                completedBytes = writeBuffer_.size();
                writeBuffer_.clear();
                writeOffset_ = 0;
                return true;
            }

            ResetEvent(writeEvent_);
            writeOverlapped_.Offset = 0;
            writeOverlapped_.OffsetHigh = 0;
            DWORD transferred = 0;
            const qsizetype remaining =
                writeBuffer_.size() - writeOffset_;
            if (WriteFile(
                    handle_,
                    writeBuffer_.constData() + writeOffset_,
                    static_cast<DWORD>(remaining),
                    &transferred,
                    &writeOverlapped_)) {
                if (transferred == 0) {
                    error = ERROR_WRITE_FAULT;
                    return false;
                }
                writeOffset_ += static_cast<qsizetype>(transferred);
                continue;
            }

            error = GetLastError();
            if (error == ERROR_IO_PENDING) {
                writePending_ = true;
                error = ERROR_SUCCESS;
                return true;
            }
            return false;
        }
        return true;
    }

private:
    void settlePendingOperation(
        OVERLAPPED& operation,
        bool& pending) noexcept
    {
        if (!pending || handle_ == INVALID_HANDLE_VALUE) {
            return;
        }

        DWORD transferred = 0;
        // TRUE waits for either normal completion or the cancellation
        // requested above. Both are terminal and release the caller-owned
        // OVERLAPPED/buffer storage.
        static_cast<void>(GetOverlappedResult(
            handle_,
            &operation,
            &transferred,
            TRUE));
        pending = false;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE readEvent_ = nullptr;
    HANDLE writeEvent_ = nullptr;
    OVERLAPPED readOverlapped_{};
    OVERLAPPED writeOverlapped_{};
    std::array<std::byte, kReadBufferSize> readBuffer_{};
    bool readPending_{};
    bool writePending_{};
    QByteArray writeBuffer_;
    qsizetype writeOffset_{};
};

struct AsynchronousOpenAttempt {
    std::atomic_bool cancelled{};
    std::atomic_bool completed{};
    std::mutex resultMutex;
    std::unique_ptr<NativeSerialPort> serial;
    detail::PortIdentitySnapshot verifiedIdentity;
    DWORD error{ERROR_SUCCESS};
    bool identityRejected{};
};

[[nodiscard]] detail::DetachedDriverOperationBudget&
detachedDriverOperationBudget()
{
    // Detached driver calls may outlive QApplication teardown. Intentionally
    // keep the tiny atomic budget alive until Windows terminates the process.
    static auto* const budget = new detail::DetachedDriverOperationBudget(
        kMaximumDetachedDriverOperations);
    return *budget;
}

class DetachedDriverOperationLease final {
public:
    explicit DetachedDriverOperationLease(
        detail::DetachedDriverOperationBudget& budget) noexcept
        : budget_(&budget)
    {
    }

    ~DetachedDriverOperationLease()
    {
        release();
    }

    DetachedDriverOperationLease(
        const DetachedDriverOperationLease&) = delete;
    DetachedDriverOperationLease& operator=(
        const DetachedDriverOperationLease&) = delete;

    void release() noexcept
    {
        if (budget_ != nullptr) {
            budget_->release();
            budget_ = nullptr;
        }
    }

private:
    detail::DetachedDriverOperationBudget* budget_;
};

void retireNativeSerialPort(
    std::unique_ptr<NativeSerialPort> serial) noexcept
{
    if (serial == nullptr) {
        return;
    }
    if (!serial->isOpen()) {
        serial.reset();
        return;
    }

    // A broken vendor driver is allowed to delay CancelIoEx/CloseHandle, but
    // it must never pin the Qt transport thread (and therefore the GUI during
    // shutdown). The heap-owned OVERLAPPED structures remain alive in this
    // cleanup thread until the driver reaches a terminal state. A shared
    // process budget prevents repeated driver failures from creating
    // unbounded detached threads.
    NativeSerialPort* const rawSerial = serial.release();
    detail::DetachedDriverOperationBudget& budget =
        detachedDriverOperationBudget();
    if (!budget.tryAcquire()) {
        // The request storage cannot be destroyed before the driver reaches a
        // terminal state. Quarantine this one object and permanently prevent
        // further opens instead of either blocking the GUI or spawning an
        // unbounded number of threads.
        budget.poison();
        static_cast<void>(rawSerial);
        return;
    }
    detail::DetachedDriverOperationBudget* const budgetPointer =
        &budget;
    try {
        std::thread([rawSerial, budgetPointer] {
            const DetachedDriverOperationLease lease(
                *budgetPointer);
            delete rawSerial;
        }).detach();
    } catch (...) {
        budget.release();
        budget.poison();
        // Keeping the handle and its OVERLAPPED storage alive is safer than
        // destroying it while a driver may still complete an operation.
        static_cast<void>(rawSerial);
    }
}

struct DesiredControl {
    protocol::MessageType type{protocol::MessageType::Hello};
    QByteArray payload;
    quint64 revision{};
    quint64 inFlightRevision{};
    quint32 inFlightSequence{};
    bool valid{};
    bool dirty{};
    bool inFlight{};
};

struct PendingAcknowledgement {
    DesiredControl* control{};
    protocol::MessageType type{protocol::MessageType::Hello};
    quint64 revision{};
    qint64 deadlineMilliseconds{};
};

[[nodiscard]] QByteArray toByteArray(
    const std::span<const protocol::Byte> bytes)
{
    return QByteArray(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<qsizetype>(bytes.size()));
}

} // namespace

class SerialTransportRuntime final : public QObject {
public:
    explicit SerialTransportRuntime(SerialTransportWorker* owner)
        : QObject(nullptr),
          owner_(owner),
          timer_(new QTimer(this))
    {
        monotonicClock_.start();
        timer_->setTimerType(Qt::PreciseTimer);
        timer_->setInterval(kIoTickMilliseconds);
        connect(timer_, &QTimer::timeout, this, [this] {
            tick();
        });

        stripControl_.type = protocol::MessageType::SetStripConfig;
        brightnessControl_.type = protocol::MessageType::SetBrightness;
        effectControl_.type = protocol::MessageType::SetEffect;
        fallbackModeControl_.type = protocol::MessageType::SetMode;
        modeControl_.type = protocol::MessageType::SetMode;

        QSettings settings;
        trustedPortNameHint_ =
            settings.value(QLatin1String(kTrustedPortNameKey))
                .toString()
                .trimmed()
                .toUpper();
        trustedPersistentIdentity_ =
            settings.value(QLatin1String(kTrustedPortIdentityKey))
                .toString();
        if (!trustedPersistentIdentity_.startsWith(
                QStringLiteral("setupapi-v1|"),
                Qt::CaseInsensitive)) {
            trustedPortNameHint_.clear();
            trustedPersistentIdentity_.clear();
            settings.remove(QLatin1String(kTrustedPortNameKey));
            settings.remove(QLatin1String(kTrustedPortIdentityKey));
        }
        settings.remove(
            QLatin1String(kLegacyTrustedPortIdentityKey));
    }

    void refreshPorts()
    {
        QString errorMessage;
        QVector<SerialPortDescriptor> ports =
            enumerateNativePorts(&errorMessage);
        publishPortRefreshFinished(
            std::move(ports),
            std::move(errorMessage));
    }

    void startTransport(const quint64 generation)
    {
        if (generation < generation_) {
            return;
        }
        generation_ = generation;
        if (!configuration_.isValid()) {
            runningRequested_ = false;
            publishStatus(
                ConnectionState::Error,
                QStringLiteral("Invalid serial transport configuration."));
            return;
        }
        if (runningRequested_) {
            return;
        }

        runningRequested_ = true;
        nextConnectionAttemptMilliseconds_ = 0;
        candidates_.clear();
        candidateIndex_ = 0;
        timer_->start();
        publishStatus(
            ConnectionState::Searching,
            QStringLiteral("Searching for an Ambilight controller."));
        tick();
    }

    void stopTransport(const quint64 generation)
    {
        if (generation < generation_) {
            return;
        }
        generation_ = generation;
        runningRequested_ = false;
        timer_->stop();
        completeDrain(false);
        clearRetainedFrame();
        closePort();
        candidates_.clear();
        candidateIndex_ = 0;
        selectedPort_.clear();
        publishStatus(
            ConnectionState::Stopped,
            QStringLiteral("Serial transport stopped."));
    }

    void updateConfiguration(
        ConnectionConfiguration configuration,
        const quint64 generation,
        const bool shouldRun)
    {
        if (generation < generation_) {
            return;
        }
        configuration.portName = configuration.portName.trimmed().toUpper();
        generation_ = generation;
        const bool changed = configuration != configuration_;
        configuration_ = std::move(configuration);

        if (!configuration_.isValid()) {
            runningRequested_ = false;
            timer_->stop();
            completeDrain(false);
            clearRetainedFrame();
            closePort();
            publishStatus(
                ConnectionState::Error,
                QStringLiteral("Invalid serial transport configuration."));
            return;
        }
        if (!shouldRun) {
            runningRequested_ = false;
            timer_->stop();
            completeDrain(false);
            closePort();
            publishStatus(
                ConnectionState::Stopped,
                QStringLiteral("Serial transport stopped."));
            return;
        }

        runningRequested_ = true;
        if (changed || !timer_->isActive()) {
            closePort();
            candidates_.clear();
            candidateIndex_ = 0;
            nextConnectionAttemptMilliseconds_ = 0;
            timer_->start();
            publishStatus(
                ConnectionState::Searching,
                QStringLiteral("Serial configuration changed; reconnecting."));
            tick();
        }
    }

    void setStripConfiguration(
        const protocol::StripConfig& configuration,
        const quint64 frameEpoch)
    {
        frameEpoch_ = std::max(frameEpoch_, frameEpoch);
        std::array<protocol::Byte, protocol::kStripConfigPayloadSize> payload{};
        if (!protocol::encodeStripConfig(configuration, payload)) {
            publishInvalidCommand(protocol::MessageType::SetStripConfig);
            return;
        }
        ambientTimeoutMilliseconds_ =
            configuration.ambientTimeoutMilliseconds;
        if (updateDesiredControl(stripControl_, toByteArray(payload))) {
            // The retained pixels are already in physical strip order. Any
            // topology/remapping change requires a newly mapped desktop
            // sample.
            clearRetainedFrame();
        }
    }

    void setMode(
        const protocol::OperatingMode mode,
        const quint64 frameEpoch)
    {
        frameEpoch_ = std::max(frameEpoch_, frameEpoch);
        std::array<protocol::Byte, protocol::kModePayloadSize> payload{};
        if (!protocol::encodeOperatingMode(mode, payload)) {
            publishInvalidCommand(protocol::MessageType::SetMode);
            return;
        }
        const std::optional<protocol::OperatingMode> previousMode =
            desiredMode_;
        desiredMode_ = mode;
        ambientFramesEnabled_ =
            mode == protocol::OperatingMode::Ambilight;
        if (!ambientFramesEnabled_) {
            clearRetainedFrame();
        }
        static_cast<void>(
            updateDesiredControl(modeControl_, toByteArray(payload)));

        if (!previousMode.has_value() || *previousMode != mode) {
            if (mode == protocol::OperatingMode::Ambilight) {
                markControlForReplay(fallbackModeControl_);
                if (desiredFallbackMode_.has_value()
                    && *desiredFallbackMode_
                        != protocol::OperatingMode::Off) {
                    markControlForReplay(effectControl_);
                }
            } else if (mode == protocol::OperatingMode::Solid
                || mode == protocol::OperatingMode::Effect) {
                markControlForReplay(effectControl_);
            }
        }
    }

    void setFallbackMode(const protocol::OperatingMode mode)
    {
        if (mode == protocol::OperatingMode::Ambilight) {
            publishInvalidCommand(protocol::MessageType::SetMode);
            return;
        }
        std::array<protocol::Byte, protocol::kModePayloadSize> payload{};
        if (!protocol::encodeOperatingMode(mode, payload)) {
            publishInvalidCommand(protocol::MessageType::SetMode);
            return;
        }
        desiredFallbackMode_ = mode;
        const bool changed = updateDesiredControl(
            fallbackModeControl_,
            toByteArray(payload));
        if (changed && modeControl_.valid) {
            ++modeControl_.revision;
            modeControl_.dirty = true;
        }
    }

    void setBrightness(const protocol::Byte brightness)
    {
        std::array<protocol::Byte, protocol::kBrightnessPayloadSize> payload{};
        if (!protocol::encodeBrightness(brightness, payload)) {
            publishInvalidCommand(protocol::MessageType::SetBrightness);
            return;
        }
        static_cast<void>(
            updateDesiredControl(brightnessControl_, toByteArray(payload)));
    }

    void setEffect(const protocol::EffectConfig& effect)
    {
        std::array<protocol::Byte, protocol::kEffectConfigPayloadSize> payload{};
        if (!protocol::encodeEffectConfig(effect, payload)) {
            publishInvalidCommand(protocol::MessageType::SetEffect);
            return;
        }
        const bool changed =
            updateDesiredControl(effectControl_, toByteArray(payload));
        // SET_EFFECT activates the effect immediately in the firmware. Replay
        // the desired mode afterwards (notably Ambilight) even if its previous
        // value was already acknowledged.
        if (changed && modeControl_.valid) {
            ++modeControl_.revision;
            modeControl_.dirty = true;
        }
    }

    void requestDeviceState()
    {
        explicitStateRequest_ = true;
    }

    void requestDrain(const int timeoutMilliseconds)
    {
        if (drainRequested_) {
            completeDrain(false);
        }
        drainRequested_ = true;
        drainDeadlineMilliseconds_ =
            monotonicClock_.elapsed()
            + std::clamp(timeoutMilliseconds, 50, 10'000);
        pendingRgbFrame_.reset();
        outbound_.clearLedFrame();
        if (!runningRequested_
            || connectionState_ != ConnectionState::Connected) {
            completeDrain(false);
            return;
        }
        setActiveIoTimer();
        tick();
    }

    void submitFrame(
        const QByteArray& rgbPixels,
        const quint64 generation,
        const quint64 frameEpoch,
        const quint64 facadeReplacedFrames)
    {
        facadeReplacedFrames_ =
            std::max(facadeReplacedFrames_, facadeReplacedFrames);
        if (generation != generation_
            || !detail::frameEpochMatches(frameEpoch_, frameEpoch)
            || !runningRequested_
            || !ambientFramesEnabled_
            || rgbPixels.isEmpty()
            || (rgbPixels.size() % 3) != 0
            || rgbPixels.size() / 3 > 300) {
            return;
        }
        frameEpoch_ = frameEpoch;
        lastRgbFrame_ = rgbPixels;
        pendingRgbFrame_ = rgbPixels;
    }

    void clearFrame(const quint64 frameEpoch)
    {
        frameEpoch_ = std::max(frameEpoch_, frameEpoch);
        clearRetainedFrame();
    }

private:
    void tick()
    {
        if (!runningRequested_) {
            return;
        }

        const qint64 now = monotonicClock_.elapsed();
        if (openAttempt_ != nullptr) {
            serviceOpenAttempt(now);
            publishStatisticsIfDue(now);
            return;
        }
        if (serial_ == nullptr || !serial_->isOpen()) {
            if (now >= nextConnectionAttemptMilliseconds_) {
                attemptConnection(now);
            }
            if (serial_ == nullptr && openAttempt_ == nullptr
                && runningRequested_ && timer_->isActive()) {
                setReconnectTimer(now);
            }
            publishStatisticsIfDue(now);
            return;
        }

        QByteArray received;
        DWORD error = ERROR_SUCCESS;
        if (!serial_->serviceRead(received, error)) {
            scheduleReconnect(
                QStringLiteral("Serial read failed."),
                error,
                now);
            return;
        }
        if (!received.isEmpty()) {
            statistics_.receivedBytes +=
                static_cast<quint64>(received.size());
            decoder_.push(
                std::span<const protocol::Byte>(
                    reinterpret_cast<const protocol::Byte*>(
                        received.constData()),
                    static_cast<std::size_t>(received.size())),
                [this, now](const protocol::PacketView& packet) {
                    handlePacket(packet, now);
                });
            if (serial_ == nullptr || !serial_->isOpen()) {
                publishStatisticsIfDue(now);
                return;
            }
        }

        bool writeCompleted = false;
        qsizetype completedBytes = 0;
        if (!serial_->serviceWrite(
                writeCompleted,
                completedBytes,
                error)) {
            scheduleReconnect(
                QStringLiteral("Serial write failed."),
                error,
                now);
            return;
        }
        if (writeCompleted) {
            statistics_.transmittedBytes +=
                static_cast<quint64>(completedBytes);
            if (activeWriteWasFrame_) {
                ++statistics_.transmittedFrames;
            }
            activeWriteWasFrame_ = false;
        }

        if (connectionState_ == ConnectionState::Handshaking) {
            if (detail::deadlineExpired(
                    now,
                    handshakeDeadlineMilliseconds_)) {
                scheduleReconnect(
                    QStringLiteral(
                        "The port did not answer the Ambilight handshake."),
                    ERROR_TIMEOUT,
                    now);
                return;
            }
            pumpHandshake(now);
        } else if (connectionState_ == ConnectionState::Connected) {
            pumpDesiredControls(now);
            if (drainRequested_) {
                pumpDrain(now);
            } else {
                if (!pumpStateRequest(now)) {
                    return;
                }
                pumpLedFrame(now);
            }
            if (now - lastValidPacketMilliseconds_
                >= configuration_.connectionTimeoutMilliseconds) {
                scheduleReconnect(
                    QStringLiteral("The controller stopped responding."),
                    ERROR_TIMEOUT,
                    now);
                return;
            }
            if (hasExpiredAcknowledgement(now)) {
                scheduleReconnect(
                    QStringLiteral(
                        "The controller did not acknowledge a command."),
                    ERROR_TIMEOUT,
                    now);
                return;
            }
        }

        if (serial_ != nullptr && !serial_->hasWriteInProgress()) {
            const bool nextIsFrame =
                outbound_.controlPacketCount() == 0
                && outbound_.hasLedFrame();
            auto next = outbound_.takeNext();
            if (next.has_value()) {
                activeWriteWasFrame_ = nextIsFrame;
                if (!serial_->beginWrite(std::move(*next))) {
                    activeWriteWasFrame_ = false;
                    scheduleReconnect(
                        QStringLiteral("Could not start a serial write."),
                        ERROR_WRITE_FAULT,
                        now);
                    return;
                }
            }
        }

        const auto& decoderStats = decoder_.stats();
        statistics_.protocolErrors =
            static_cast<quint64>(decoderStats.crcErrors)
            + decoderStats.cobsErrors
            + decoderStats.formatErrors;
        statistics_.replacedFrames =
            facadeReplacedFrames_ + outbound_.replacedFrameCount();
        publishStatisticsIfDue(now);
    }

    void attemptConnection(const qint64 now)
    {
        if (candidates_.isEmpty()
            || candidateIndex_ >= candidates_.size()) {
            QString enumerationError;
            const QVector<NativePortRecord> available =
                enumerateNativePortRecords(&enumerationError);
            QVector<SerialPortDescriptor> descriptors;
            descriptors.reserve(available.size());
            for (const NativePortRecord& record : available) {
                descriptors.push_back(record.descriptor);
            }
            publishPorts(std::move(descriptors));

            candidates_.clear();
            candidateIndex_ = 0;
            if (configuration_.selectionMode
                == PortSelectionMode::Explicit) {
                const auto selected = std::find_if(
                    available.cbegin(),
                    available.cend(),
                    [this](const NativePortRecord& port) {
                        return port.descriptor.portName.compare(
                                   configuration_.portName,
                                   Qt::CaseInsensitive)
                            == 0;
                    });
                if (selected != available.cend()
                    && selected->identity.classified
                    && !selected->identity
                            .transientFingerprint.isEmpty()) {
                    candidates_.push_back(*selected);
                }
            } else {
                // Never probe arbitrary COM ports. A first connection must be
                // selected manually; after a successful Ambilight handshake,
                // Automatic may open only the persistent SetupAPI identity
                // verified by that manual handshake. COM is merely a hint:
                // Windows may assign a different number after reconnecting.
                if (!trustedPersistentIdentity_.isEmpty()) {
                    const auto confirmed = std::find_if(
                        available.cbegin(),
                        available.cend(),
                        [this](const NativePortRecord& port) {
                            return !port.identity.bluetooth
                                && port.identity.automaticEligible
                                && detail::
                                    trustedPersistentIdentityMatches(
                                        port.identity.persistentIdentity,
                                        trustedPersistentIdentity_);
                        });
                    if (confirmed != available.cend()) {
                        candidates_.push_back(*confirmed);
                    }
                }
            }

            if (candidates_.isEmpty()) {
                QString message;
                if (!enumerationError.isEmpty()) {
                    message = enumerationError;
                } else if (available.isEmpty()) {
                    message = QStringLiteral(
                        "No COM ports are available.");
                } else if (configuration_.selectionMode
                    == PortSelectionMode::Automatic) {
                    message = trustedPersistentIdentity_.isEmpty()
                        ? QStringLiteral(
                              "Automatic connection requires one manual "
                              "Ambilight connection to a USB-UART with a "
                              "unique hardware serial identity.")
                        : QStringLiteral(
                              "The previously confirmed Ambilight controller "
                              "identity is unavailable; select its current "
                              "COM port manually to verify it again.");
                } else {
                    message = QStringLiteral(
                        "The selected COM port is unavailable.");
                }
                publishStatus(
                    ConnectionState::Searching,
                    message);
                nextConnectionAttemptMilliseconds_ =
                    now
                    + configuration_.reconnectIntervalMilliseconds;
                return;
            }
        }

        const NativePortRecord candidate =
            candidates_[candidateIndex_++];
        if (candidate.identity.bluetooth) {
            runningRequested_ = false;
            timer_->stop();
            completeDrain(false);
            publishStatus(
                ConnectionState::Error,
                QStringLiteral(
                    "Bluetooth/RFCOMM serial ports are not supported; "
                    "select the controller USB-UART COM port."),
                ERROR_NOT_SUPPORTED);
            return;
        }
        selectedPort_ = candidate.descriptor.portName;
        selectedIdentity_ = candidate.identity;
        detail::DetachedDriverOperationBudget& operationBudget =
            detachedDriverOperationBudget();
        if (operationBudget.isPoisoned()) {
            runningRequested_ = false;
            timer_->stop();
            completeDrain(false);
            publishStatus(
                ConnectionState::Error,
                QStringLiteral(
                    "Windows serial driver cleanup could not be bounded; "
                    "restart the application before opening another COM port."),
                ERROR_NOT_ENOUGH_QUOTA);
            return;
        }
        if (!operationBudget.tryAcquire()) {
            candidates_.clear();
            candidateIndex_ = 0;
            nextConnectionAttemptMilliseconds_ =
                now + configuration_.reconnectIntervalMilliseconds;
            setReconnectTimer(now);
            publishStatus(
                ConnectionState::Reconnecting,
                QStringLiteral(
                    "Waiting for an earlier Windows serial driver operation "
                    "to finish before opening %1.")
                    .arg(selectedPort_),
                ERROR_BUSY);
            return;
        }

        ++reconnectAttempt_;
        publishStatus(
            ConnectionState::Opening,
            QStringLiteral("Opening %1.").arg(selectedPort_));

        const QString portName = selectedPort_;
        const int baudRate = configuration_.baudRate;
        const PortSelectionMode selectionMode =
            configuration_.selectionMode;
        const detail::PortIdentitySnapshot expectedIdentity =
            selectedIdentity_;
        detail::DetachedDriverOperationBudget* const
            operationBudgetPointer = &operationBudget;
        std::shared_ptr<AsynchronousOpenAttempt> attempt;
        bool operationReserved = true;
        try {
            attempt =
                std::make_shared<AsynchronousOpenAttempt>();
            openAttempt_ = attempt;
            openAttemptTimedOut_ = false;
            openAttemptDeadlineMilliseconds_ =
                now + kPortOpenTimeoutMilliseconds;
            std::thread(
                [attempt,
                 portName,
                 baudRate,
                 selectionMode,
                 expectedIdentity,
                 operationBudgetPointer]() mutable {
                    const DetachedDriverOperationLease operationLease(
                        *operationBudgetPointer);
                    DWORD error = ERROR_SUCCESS;
                    bool opened = false;
                    bool identityRejected = false;
                    detail::PortIdentitySnapshot verifiedIdentity;
                    std::unique_ptr<NativeSerialPort> serial;
                    try {
                        serial =
                            std::make_unique<NativeSerialPort>();
                        opened =
                            serial->open(portName, baudRate, error);
                        if (opened) {
                            QString enumerationError;
                            const QVector<NativePortRecord> currentPorts =
                                enumerateNativePortRecords(
                                    &enumerationError);
                            const auto current = std::find_if(
                                currentPorts.cbegin(),
                                currentPorts.cend(),
                                [&portName](
                                    const NativePortRecord& port) {
                                    return port.descriptor.portName.compare(
                                               portName,
                                               Qt::CaseInsensitive)
                                        == 0;
                                });
                            if (current == currentPorts.cend()
                                || !detail::postOpenPortIdentityMatches(
                                    selectionMode,
                                    expectedIdentity,
                                    current->identity)) {
                                opened = false;
                                identityRejected = true;
                                error = ERROR_DEVICE_NOT_CONNECTED;
                            } else {
                                verifiedIdentity = current->identity;
                            }
                        }
                    } catch (...) {
                        error = ERROR_NOT_ENOUGH_MEMORY;
                        opened = false;
                    }
                    const bool cancelled =
                        attempt->cancelled.load(
                            std::memory_order_acquire);
                    {
                        const std::lock_guard lock(
                            attempt->resultMutex);
                        if (opened && !cancelled) {
                            attempt->serial = std::move(serial);
                            attempt->verifiedIdentity =
                                std::move(verifiedIdentity);
                        }
                        attempt->error = cancelled
                            ? ERROR_CANCELLED
                            : (opened ? ERROR_SUCCESS : error);
                        attempt->identityRejected =
                            !cancelled && identityRejected;
                    }
                    attempt->completed.store(
                        true,
                        std::memory_order_release);
                    // If the runtime abandoned this attempt, release its last
                    // shared owner while the detached-operation lease is still
                    // active. Any contained serial handle is then retired on
                    // this background thread, never on the Qt thread.
                    attempt.reset();
                })
                .detach();
            operationReserved = false;
        } catch (...) {
            if (operationReserved) {
                operationBudget.release();
            }
            if (attempt != nullptr) {
                attempt->cancelled.store(
                    true,
                    std::memory_order_release);
            }
            openAttempt_.reset();
            scheduleReconnect(
                QStringLiteral(
                    "Could not start a bounded open for %1.")
                    .arg(selectedPort_),
                ERROR_NOT_ENOUGH_MEMORY,
                now);
            return;
        }
        setOpenPollTimer();
    }

    void serviceOpenAttempt(const qint64 now)
    {
        const std::shared_ptr<AsynchronousOpenAttempt> attempt =
            openAttempt_;
        if (attempt == nullptr) {
            return;
        }
        if (!attempt->completed.load(std::memory_order_acquire)) {
            if (!openAttemptTimedOut_
                && detail::deadlineExpired(
                    now,
                    openAttemptDeadlineMilliseconds_)) {
                openAttemptTimedOut_ = true;
                attempt->cancelled.store(
                    true,
                    std::memory_order_release);
                completeDrain(false);
                publishStatus(
                    ConnectionState::Reconnecting,
                    QStringLiteral(
                        "Opening %1 timed out; waiting for the Windows "
                        "serial driver to return safely.")
                        .arg(selectedPort_),
                    ERROR_TIMEOUT);
            }
            setOpenPollTimer(openAttemptTimedOut_ ? 100 : kOpenPollMilliseconds);
            return;
        }

        std::unique_ptr<NativeSerialPort> openedSerial;
        detail::PortIdentitySnapshot verifiedIdentity;
        DWORD error = ERROR_SUCCESS;
        bool identityRejected = false;
        {
            const std::lock_guard lock(attempt->resultMutex);
            openedSerial = std::move(attempt->serial);
            verifiedIdentity =
                std::move(attempt->verifiedIdentity);
            error = attempt->error;
            identityRejected = attempt->identityRejected;
        }
        const bool timedOut = openAttemptTimedOut_;
        openAttempt_.reset();
        openAttemptTimedOut_ = false;

        if (!runningRequested_) {
            retireNativeSerialPort(std::move(openedSerial));
            return;
        }
        if (timedOut || openedSerial == nullptr) {
            retireNativeSerialPort(std::move(openedSerial));
            scheduleReconnect(
                timedOut
                    ? QStringLiteral("Opening %1 timed out.")
                          .arg(selectedPort_)
                    : identityRejected
                    ? QStringLiteral(
                          "The device behind %1 changed while it was being "
                          "opened; no Ambilight probe was sent.")
                          .arg(selectedPort_)
                    : QStringLiteral("Could not open %1.")
                          .arg(selectedPort_),
                timedOut ? ERROR_TIMEOUT : error,
                now);
            return;
        }

        selectedIdentity_ = std::move(verifiedIdentity);
        serial_ = std::move(openedSerial);
        beginHandshake(now);
    }

    void beginHandshake(const qint64 now)
    {
        setActiveIoTimer();
        decoder_.reset();
        outbound_.clear();
        pendingAcknowledgements_.clear();
        stateRequestSequences_.clear();
        stateRequestDeadlineMilliseconds_ = 0;
        lastDeviceUptimeMilliseconds_.reset();
        activeWriteWasFrame_ = false;
        resetDesiredControlsForConnection();

        const quint32 sequence = nextSequence();
        const auto hello = encodeHelloPacket(sequence);
        if (!hello.has_value()) {
            scheduleReconnect(
                QStringLiteral("Could not encode the handshake."),
                ERROR_INVALID_DATA,
                now);
            return;
        }
        helloSequence_ = sequence;
        handshakePacket_ = *hello;
        nextHandshakeAttemptMilliseconds_ =
            now + kHandshakeSettleMilliseconds;
        handshakeDeadlineMilliseconds_ =
            nextHandshakeAttemptMilliseconds_
            + configuration_.handshakeTimeoutMilliseconds;
        connectionState_ = ConnectionState::Handshaking;
        publishStatus(
            ConnectionState::Handshaking,
            QStringLiteral(
                "Waiting for an Ambilight controller on %1.")
                .arg(selectedPort_));
    }

    void pumpHandshake(const qint64 now)
    {
        if (handshakePacket_.isEmpty()
            || !detail::handshakeAttemptDue(
                now,
                nextHandshakeAttemptMilliseconds_,
                outbound_.controlPacketCount() == 0,
                serial_ != nullptr
                    && serial_->hasWriteInProgress())) {
            return;
        }
        outbound_.pushControl(handshakePacket_);
        nextHandshakeAttemptMilliseconds_ =
            now + kHandshakeRetryMilliseconds;
    }

    void scheduleReconnect(
        const QString& reason,
        const DWORD error,
        const qint64 now)
    {
        closePort();

        const bool hasAnotherCandidate =
            candidateIndex_ < candidates_.size();
        if (!hasAnotherCandidate) {
            candidates_.clear();
            candidateIndex_ = 0;
        }
        nextConnectionAttemptMilliseconds_ =
            now
            + configuration_.reconnectIntervalMilliseconds;
        setReconnectTimer(now);
        publishStatus(
            ConnectionState::Reconnecting,
            reason,
            error);
    }

    void closePort()
    {
        completeDrain(false);
        std::unique_ptr<NativeSerialPort> completedOpenSerial;
        if (openAttempt_ != nullptr) {
            const std::shared_ptr<AsynchronousOpenAttempt> attempt =
                std::move(openAttempt_);
            attempt->cancelled.store(
                true,
                std::memory_order_release);
            // A completed attempt may already own an open handle, while its
            // queued service event has not run yet. Extract that handle before
            // dropping the shared state so its destructor can never block the
            // Qt transport thread.
            if (attempt->completed.load(std::memory_order_acquire)) {
                const std::lock_guard lock(attempt->resultMutex);
                completedOpenSerial =
                    std::move(attempt->serial);
            }
        }
        openAttemptTimedOut_ = false;
        retireNativeSerialPort(std::move(completedOpenSerial));
        retireNativeSerialPort(std::move(serial_));
        decoder_.reset();
        outbound_.clear();
        handshakePacket_.clear();
        pendingAcknowledgements_.clear();
        stateRequestSequences_.clear();
        stateRequestDeadlineMilliseconds_ = 0;
        lastDeviceUptimeMilliseconds_.reset();
        activeWriteWasFrame_ = false;
        connectionState_ = ConnectionState::Stopped;
        for (DesiredControl* control : desiredControls()) {
            control->inFlight = false;
            control->inFlightSequence = 0;
            control->inFlightRevision = 0;
            if (control->valid) {
                control->dirty = true;
            }
        }
    }

    void setActiveIoTimer()
    {
        if (timer_->timerType() != Qt::PreciseTimer) {
            timer_->setTimerType(Qt::PreciseTimer);
        }
        if (timer_->interval() != kIoTickMilliseconds) {
            timer_->setInterval(kIoTickMilliseconds);
        }
        if (runningRequested_ && !timer_->isActive()) {
            timer_->start();
        }
    }

    void setOpenPollTimer(
        const int intervalMilliseconds = kOpenPollMilliseconds)
    {
        if (timer_->timerType() != Qt::CoarseTimer) {
            timer_->setTimerType(Qt::CoarseTimer);
        }
        if (timer_->interval() != intervalMilliseconds) {
            timer_->setInterval(intervalMilliseconds);
        }
        if (runningRequested_ && !timer_->isActive()) {
            timer_->start();
        }
    }

    void setReconnectTimer(const qint64 now)
    {
        const int interval =
            detail::reconnectTimerIntervalMilliseconds(
                now,
                nextConnectionAttemptMilliseconds_);
        if (timer_->timerType() != Qt::CoarseTimer) {
            timer_->setTimerType(Qt::CoarseTimer);
        }
        if (timer_->interval() != interval) {
            timer_->setInterval(interval);
        }
        if (runningRequested_ && !timer_->isActive()) {
            timer_->start();
        }
    }

    [[nodiscard]] bool acceptDeviceUptime(
        const quint32 uptimeMilliseconds,
        const qint64 now)
    {
        if (lastDeviceUptimeMilliseconds_.has_value()
            && detail::deviceUptimeIndicatesReset(
                *lastDeviceUptimeMilliseconds_,
                uptimeMilliseconds)) {
            scheduleReconnect(
                QStringLiteral(
                    "The controller restarted; beginning a new host session "
                    "and replaying the desired configuration."),
                ERROR_DEVICE_REINITIALIZATION_NEEDED,
                now);
            return false;
        }
        lastDeviceUptimeMilliseconds_ = uptimeMilliseconds;
        return true;
    }

    void handlePacket(
        const protocol::PacketView& packet,
        const qint64 now)
    {
        if (packet.version != protocol::kProtocolVersion) {
            return;
        }

        if (connectionState_ == ConnectionState::Handshaking) {
            if (packet.type != protocol::MessageType::DeviceInfo
                || packet.sequence != helloSequence_) {
                return;
            }

            protocol::DeviceInfo info;
            if (!protocol::decodeDeviceInfo(packet.payload, info)
                || info.protocolVersion
                    != protocol::kProtocolVersion
                || (info.capabilities
                    & protocol::CapabilityRgb8Frames) == 0) {
                scheduleReconnect(
                    QStringLiteral(
                        "The device reports an incompatible Ambilight "
                        "protocol or lacks RGB frame support."),
                    ERROR_NOT_SUPPORTED,
                    now);
                return;
            }

            deviceInformation_ = info;
            lastDeviceUptimeMilliseconds_ =
                info.uptimeMilliseconds;
            const bool manualVerification =
                configuration_.selectionMode
                == PortSelectionMode::Explicit;
            if (manualVerification) {
                QSettings settings;
                if (selectedIdentity_.automaticEligible
                    && !selectedIdentity_
                            .persistentIdentity.isEmpty()) {
                    trustedPortNameHint_ = selectedPort_;
                    trustedPersistentIdentity_ =
                        selectedIdentity_.persistentIdentity;
                    settings.setValue(
                        QLatin1String(kTrustedPortNameKey),
                        trustedPortNameHint_);
                    settings.setValue(
                        QLatin1String(kTrustedPortIdentityKey),
                        trustedPersistentIdentity_);
                } else {
                    // Do not silently retain trust in a different controller.
                    // Bridges without a unique serial (common on CH340 clones)
                    // remain available in explicit/manual mode only.
                    trustedPortNameHint_.clear();
                    trustedPersistentIdentity_.clear();
                    settings.remove(
                        QLatin1String(kTrustedPortNameKey));
                    settings.remove(
                        QLatin1String(kTrustedPortIdentityKey));
                }
            }
            candidates_.clear();
            candidateIndex_ = 0;
            lastValidPacketMilliseconds_ = now;
            lastStatePollMilliseconds_ = 0;
            explicitStateRequest_ = true;
            connectionState_ = ConnectionState::Connected;
            publishDeviceInformation(info);
            QString connectedMessage = QStringLiteral(
                "Connected to Ambilight firmware %1.%2.%3 on %4.")
                .arg(info.firmwareMajor)
                .arg(info.firmwareMinor)
                .arg(info.firmwarePatch)
                .arg(selectedPort_);
            if (manualVerification
                && !selectedIdentity_.automaticEligible) {
                connectedMessage += QStringLiteral(
                    " Automatic reconnect is disabled because this "
                    "USB-UART does not expose a conservative unique serial "
                    "identity; keep using manual COM selection.");
            }
            publishStatus(
                ConnectionState::Connected,
                connectedMessage);
            return;
        }

        if (connectionState_ != ConnectionState::Connected) {
            return;
        }

        switch (packet.type) {
        case protocol::MessageType::State: {
            protocol::StateSnapshot state;
            if (!protocol::decodeState(packet.payload, state)) {
                return;
            }
            if (!stateRequestSequences_.contains(packet.sequence)) {
                // STATE is a direct response to GET_STATE. A delayed packet
                // from a completed cycle must not satisfy the current retry
                // budget or extend connection liveness.
                return;
            }
            stateRequestSequences_.clear();
            stateRequestDeadlineMilliseconds_ = 0;
            if (!acceptDeviceUptime(
                    state.uptimeMilliseconds,
                    now)) {
                return;
            }
            lastValidPacketMilliseconds_ = now;
            publishDeviceState(state);
            break;
        }
        case protocol::MessageType::Pong:
            lastValidPacketMilliseconds_ = now;
            break;
        case protocol::MessageType::Ack: {
            protocol::AckPayload acknowledgement;
            if (!protocol::decodeAck(
                    packet.payload,
                    acknowledgement)) {
                return;
            }
            handleAcknowledgement(packet, acknowledgement, now);
            break;
        }
        case protocol::MessageType::Error: {
            protocol::ErrorPayload error;
            if (!protocol::decodeError(packet.payload, error)) {
                return;
            }
            handleDeviceError(packet, error, now);
            break;
        }
        case protocol::MessageType::DeviceInfo: {
            if (packet.sequence != helloSequence_) {
                return;
            }
            protocol::DeviceInfo info;
            if (!protocol::decodeDeviceInfo(packet.payload, info)
                || info.protocolVersion
                    != protocol::kProtocolVersion
                || (info.capabilities
                    & protocol::CapabilityRgb8Frames) == 0) {
                scheduleReconnect(
                    QStringLiteral(
                        "The controller reported an incompatible protocol "
                        "or no longer supports RGB frames."),
                    ERROR_NOT_SUPPORTED,
                    now);
                return;
            }
            if (!acceptDeviceUptime(
                    info.uptimeMilliseconds,
                    now)) {
                return;
            }
            lastValidPacketMilliseconds_ = now;
            deviceInformation_ = info;
            publishDeviceInformation(info);
            break;
        }
        case protocol::MessageType::Hello:
        case protocol::MessageType::Ping:
        case protocol::MessageType::GetState:
        case protocol::MessageType::SetStripConfig:
        case protocol::MessageType::SetMode:
        case protocol::MessageType::SetBrightness:
        case protocol::MessageType::SetEffect:
        case protocol::MessageType::SaveConfig:
        case protocol::MessageType::LedFrame:
            break;
        }
    }

    void handleAcknowledgement(
        const protocol::PacketView& packet,
        const protocol::AckPayload& acknowledgement,
        const qint64 now)
    {
        const auto found =
            pendingAcknowledgements_.find(packet.sequence);
        if (found == pendingAcknowledgements_.end()
            || found->second.type
                != acknowledgement.acknowledgedType) {
            return;
        }

        lastValidPacketMilliseconds_ = now;
        PendingAcknowledgement pending = found->second;
        pendingAcknowledgements_.erase(found);
        if (pending.control != nullptr) {
            pending.control->inFlight = false;
            if (pending.control->revision == pending.revision) {
                pending.control->dirty = false;
            }
        }
        publishCommandResult({
            static_cast<int>(acknowledgement.acknowledgedType),
            packet.sequence,
            acknowledgement.status == 0,
            0,
            acknowledgement.status,
            acknowledgement.status == 0
                ? QStringLiteral("Command acknowledged.")
                : QStringLiteral(
                    "Command returned a non-zero status."),
        });
        if (acknowledgement.status != 0) {
            scheduleReconnect(
                QStringLiteral(
                    "The controller did not accept a configuration command."),
                ERROR_INVALID_DATA,
                now);
        }
    }

    void handleDeviceError(
        const protocol::PacketView& packet,
        const protocol::ErrorPayload& error,
        const qint64 now)
    {
        lastValidPacketMilliseconds_ = now;
        const auto found =
            pendingAcknowledgements_.find(packet.sequence);
        if (found != pendingAcknowledgements_.end()) {
            PendingAcknowledgement pending = found->second;
            pendingAcknowledgements_.erase(found);
            if (pending.control != nullptr) {
                pending.control->inFlight = false;
                if (pending.control->revision == pending.revision) {
                    pending.control->dirty = false;
                }
            }
        }
        publishCommandResult({
            static_cast<int>(error.failedType),
            packet.sequence,
            false,
            static_cast<int>(error.code),
            error.detail,
            QStringLiteral("The controller rejected the command."),
        });
        scheduleReconnect(
            QStringLiteral("The controller rejected a protocol packet."),
            ERROR_INVALID_DATA,
            now);
    }

    [[nodiscard]] bool updateDesiredControl(
        DesiredControl& control,
        QByteArray payload)
    {
        if (control.valid && control.payload == payload) {
            return false;
        }
        // Do not let an already queued ambient frame overtake a new
        // configuration/mode command. A write already owned by the OS is the
        // only bounded exception.
        outbound_.clearLedFrame();
        pendingRgbFrame_.reset();
        control.payload = std::move(payload);
        ++control.revision;
        control.valid = true;
        control.dirty = true;
        return true;
    }

    void markControlForReplay(DesiredControl& control)
    {
        if (!control.valid) {
            return;
        }
        ++control.revision;
        control.dirty = true;
    }

    [[nodiscard]] std::array<DesiredControl*, 5> desiredControls()
    {
        // Configuration must precede all commands that depend on it. Mode is
        // intentionally last so an effect/config is ready before activation.
        return {
            &stripControl_,
            &brightnessControl_,
            &effectControl_,
            &fallbackModeControl_,
            &modeControl_,
        };
    }

    void resetDesiredControlsForConnection()
    {
        for (DesiredControl* control : desiredControls()) {
            control->inFlight = false;
            control->inFlightSequence = 0;
            control->inFlightRevision = 0;
            if (control->valid) {
                control->dirty = true;
            }
        }
    }

    void pumpDesiredControls(const qint64 now)
    {
        // Keep exactly one reliable state-changing command in flight. Besides
        // preserving dependencies, this lets a newly selected mode supersede
        // every not-yet-sent historical control after the current ACK.
        if (!pendingAcknowledgements_.empty()) {
            return;
        }

        for (DesiredControl* control : controlsForCurrentMode()) {
            if (control == nullptr) {
                continue;
            }
            if (!control->valid
                || !control->dirty
                || control->inFlight) {
                continue;
            }

            const quint32 sequence = nextSequence();
            const auto packet = encodeProtocolPacket(
                control->type,
                protocol::PacketFlagAckRequested,
                sequence,
                control->payload);
            if (!packet.has_value()) {
                publishInvalidCommand(control->type);
                control->dirty = false;
                continue;
            }

            outbound_.pushControl(*packet);
            control->inFlight = true;
            control->inFlightSequence = sequence;
            control->inFlightRevision = control->revision;
            pendingAcknowledgements_.emplace(
                sequence,
                PendingAcknowledgement{
                    control,
                    control->type,
                    control->revision,
                    now + kAckTimeoutMilliseconds,
                });
            return;
        }
    }

    [[nodiscard]] bool pumpStateRequest(const qint64 now)
    {
        if (!stateRequestSequences_.empty()) {
            const detail::StateRequestDeadlineAction action =
                detail::stateRequestDeadlineAction(
                    now,
                    stateRequestDeadlineMilliseconds_,
                    static_cast<int>(
                        stateRequestSequences_.size()),
                    static_cast<int>(
                        detail::StateRequestSequenceWindow::
                            kCapacity));
            if (action
                == detail::StateRequestDeadlineAction::Wait) {
                return true;
            }
            if (action
                == detail::StateRequestDeadlineAction::Reconnect) {
                scheduleReconnect(
                    QStringLiteral(
                        "The controller did not answer state requests."),
                    ERROR_TIMEOUT,
                    now);
                return false;
            }
            return queueStateRequest(now);
        }

        const bool periodicDue =
            now - lastStatePollMilliseconds_
            >= configuration_.statePollIntervalMilliseconds;
        if (!explicitStateRequest_ && !periodicDue) {
            return true;
        }

        stateRequestSequences_.clear();
        return queueStateRequest(now);
    }

    [[nodiscard]] bool queueStateRequest(const qint64 now)
    {
        const quint32 sequence = nextSequence();
        const auto packet = encodeGetStatePacket(sequence);
        if (!packet.has_value()) {
            explicitStateRequest_ = true;
            return true;
        }
        if (!stateRequestSequences_.append(sequence)) {
            scheduleReconnect(
                QStringLiteral(
                    "The state-request correlation window was exhausted."),
                ERROR_INVALID_DATA,
                now);
            return false;
        }
        outbound_.pushControl(*packet);
        stateRequestDeadlineMilliseconds_ =
            now + kStateRequestTimeoutMilliseconds;
        explicitStateRequest_ = false;
        lastStatePollMilliseconds_ = now;
        return true;
    }

    void pumpDrain(const qint64 now)
    {
        if (!drainRequested_) {
            return;
        }
        if (detail::deadlineExpired(
                now,
                drainDeadlineMilliseconds_)) {
            completeDrain(false);
            return;
        }

        const auto controls = controlsForCurrentMode();
        const bool controlsSettled = std::all_of(
            controls.begin(),
            controls.end(),
            [](const DesiredControl* control) {
                return control == nullptr
                    || !control->valid
                    || (!control->dirty && !control->inFlight);
            });
        if (controlsSettled
            && pendingAcknowledgements_.empty()
            && outbound_.controlPacketCount() == 0
            && serial_ != nullptr
            && !serial_->hasWriteInProgress()) {
            completeDrain(true);
        }
    }

    void completeDrain(const bool success)
    {
        if (!drainRequested_) {
            return;
        }
        drainRequested_ = false;
        drainDeadlineMilliseconds_ = 0;
        const QPointer<SerialTransportWorker> owner = owner_;
        const quint64 generation = generation_;
        QMetaObject::invokeMethod(
            owner_.data(),
            [owner, generation, success] {
                if (owner != nullptr
                    && owner->commandGeneration_.load(
                           std::memory_order_relaxed)
                        == generation) {
                    emit owner->drainFinished(success);
                }
            },
            Qt::QueuedConnection);
    }

    [[nodiscard]] bool controlsSynchronized() const noexcept
    {
        if (!ambientFramesEnabled_
            || !desiredMode_.has_value()
            || *desiredMode_ != protocol::OperatingMode::Ambilight
            || !stripControl_.valid
            || !modeControl_.valid) {
            return false;
        }
        const auto controls = controlsForCurrentMode();
        return std::all_of(
            controls.begin(),
            controls.end(),
            [](const DesiredControl* control) {
                return control == nullptr
                    || !control->valid
                    || (!control->dirty && !control->inFlight);
            });
    }

    [[nodiscard]] std::array<DesiredControl*, 5>
    controlsForCurrentMode() noexcept
    {
        const bool ambient =
            desiredMode_ == protocol::OperatingMode::Ambilight;
        const bool standalone =
            desiredMode_ == protocol::OperatingMode::Solid
            || desiredMode_ == protocol::OperatingMode::Effect;
        if (desiredMode_ == protocol::OperatingMode::Off) {
            // A shutdown request is safety-sensitive: apply it before
            // non-visual configuration that can safely follow while dark.
            return {
                &modeControl_,
                &stripControl_,
                &brightnessControl_,
                nullptr,
                nullptr,
            };
        }
        const bool effectFallback =
            ambient
            && desiredFallbackMode_.has_value()
            && *desiredFallbackMode_ != protocol::OperatingMode::Off;
        return {
            &stripControl_,
            &brightnessControl_,
            (standalone || effectFallback) ? &effectControl_ : nullptr,
            ambient ? &fallbackModeControl_ : nullptr,
            &modeControl_,
        };
    }

    [[nodiscard]] std::array<const DesiredControl*, 5>
    controlsForCurrentMode() const noexcept
    {
        const bool ambient =
            desiredMode_ == protocol::OperatingMode::Ambilight;
        const bool standalone =
            desiredMode_ == protocol::OperatingMode::Solid
            || desiredMode_ == protocol::OperatingMode::Effect;
        if (desiredMode_ == protocol::OperatingMode::Off) {
            return {
                &modeControl_,
                &stripControl_,
                &brightnessControl_,
                nullptr,
                nullptr,
            };
        }
        const bool effectFallback =
            ambient
            && desiredFallbackMode_.has_value()
            && *desiredFallbackMode_ != protocol::OperatingMode::Off;
        return {
            &stripControl_,
            &brightnessControl_,
            (standalone || effectFallback) ? &effectControl_ : nullptr,
            ambient ? &fallbackModeControl_ : nullptr,
            &modeControl_,
        };
    }

    void pumpLedFrame(const qint64 now)
    {
        if (!controlsSynchronized()) {
            return;
        }

        const bool keepAliveDue =
            !lastRgbFrame_.isEmpty()
            && now - lastFrameQueuedMilliseconds_
                >= effectiveFrameKeepAliveMilliseconds();
        if (!pendingRgbFrame_.has_value() && !keepAliveDue) {
            return;
        }

        const QByteArray pixels =
            pendingRgbFrame_.has_value()
            ? std::move(*pendingRgbFrame_)
            : lastRgbFrame_;
        pendingRgbFrame_.reset();
        const auto packet =
            encodeLedFramePacket(nextSequence(), pixels);
        if (!packet.has_value()) {
            return;
        }
        outbound_.replaceLedFrame(*packet);
        lastFrameQueuedMilliseconds_ = now;
    }

    [[nodiscard]] int effectiveFrameKeepAliveMilliseconds() const noexcept
    {
        const int leaseDerivedInterval = std::max(
            25,
            ambientTimeoutMilliseconds_ / 3);
        return std::min(
            configuration_.frameKeepAliveIntervalMilliseconds,
            leaseDerivedInterval);
    }

    void clearRetainedFrame()
    {
        pendingRgbFrame_.reset();
        lastRgbFrame_.clear();
        outbound_.clearLedFrame();
        lastFrameQueuedMilliseconds_ = 0;
    }

    [[nodiscard]] bool hasExpiredAcknowledgement(
        const qint64 now)
    {
        for (const auto& [sequence, pending] :
             pendingAcknowledgements_) {
            if (now < pending.deadlineMilliseconds) {
                continue;
            }
            publishCommandResult({
                static_cast<int>(pending.type),
                sequence,
                false,
                static_cast<int>(protocol::ErrorCode::Busy),
                0,
                QStringLiteral("Command acknowledgement timed out."),
            });
            return true;
        }
        return false;
    }

    [[nodiscard]] quint32 nextSequence() noexcept
    {
        ++sequence_;
        if (sequence_ == 0) {
            ++sequence_;
        }
        return sequence_;
    }

    void publishInvalidCommand(const protocol::MessageType type)
    {
        publishCommandResult({
            static_cast<int>(type),
            0,
            false,
            static_cast<int>(protocol::ErrorCode::InvalidPayload),
            0,
            QStringLiteral("The command payload is invalid."),
        });
    }

    void publishStatus(
        const ConnectionState state,
        const QString& message,
        const DWORD nativeError = ERROR_SUCCESS)
    {
        connectionState_ = state;
        ConnectionStatus status{
            state,
            selectedPort_,
            message,
            nativeError == ERROR_SUCCESS ? 0U : nativeError,
            reconnectAttempt_,
        };
        if (status == lastStatus_
            && lastStatusGeneration_ == generation_) {
            return;
        }
        lastStatus_ = status;
        lastStatusGeneration_ = generation_;

        const QPointer<SerialTransportWorker> owner = owner_;
        const quint64 generation = generation_;
        QMetaObject::invokeMethod(
            owner_.data(),
            [owner, generation, status = std::move(status)] {
                if (owner != nullptr
                    && owner->commandGeneration_.load(
                           std::memory_order_relaxed)
                        == generation) {
                    emit owner->statusChanged(status);
                }
            },
            Qt::QueuedConnection);
    }

    void publishPorts(QVector<SerialPortDescriptor> ports)
    {
        const QPointer<SerialTransportWorker> owner = owner_;
        QMetaObject::invokeMethod(
            owner_.data(),
            [owner, ports = std::move(ports)] {
                if (owner != nullptr) {
                    emit owner->portsChanged(ports);
                }
            },
            Qt::QueuedConnection);
    }

    void publishPortRefreshFinished(
        QVector<SerialPortDescriptor> ports,
        QString errorMessage)
    {
        const QPointer<SerialTransportWorker> owner = owner_;
        QMetaObject::invokeMethod(
            owner_.data(),
            [owner,
             ports = std::move(ports),
             errorMessage = std::move(errorMessage)] {
                if (owner != nullptr) {
                    emit owner->portRefreshFinished(
                        ports,
                        errorMessage);
                }
            },
            Qt::QueuedConnection);
    }

    void publishDeviceInformation(const protocol::DeviceInfo& info)
    {
        const DeviceInformation information{
            info.firmwareMajor,
            info.firmwareMinor,
            info.firmwarePatch,
            info.protocolVersion,
            info.maxLedCount,
            info.configuredLedCount,
            info.capabilities,
            info.uptimeMilliseconds,
        };
        const QPointer<SerialTransportWorker> owner = owner_;
        const quint64 generation = generation_;
        QMetaObject::invokeMethod(
            owner_.data(),
            [owner, generation, information] {
                if (owner != nullptr
                    && owner->commandGeneration_.load(
                           std::memory_order_relaxed)
                        == generation) {
                    emit owner->deviceInformationChanged(information);
                }
            },
            Qt::QueuedConnection);
    }

    void publishDeviceState(const protocol::StateSnapshot& state)
    {
        const DeviceState deviceState{
            static_cast<int>(state.mode),
            static_cast<int>(state.effect),
            state.brightness,
            state.flags,
            state.ledCount,
            state.currentLimitMilliamps,
            state.lastReceivedSequence,
            state.lastAppliedSequence,
            state.crcErrors,
            state.droppedFrames,
            state.uptimeMilliseconds,
        };
        const QPointer<SerialTransportWorker> owner = owner_;
        const quint64 generation = generation_;
        QMetaObject::invokeMethod(
            owner_.data(),
            [owner, generation, deviceState] {
                if (owner != nullptr
                    && owner->commandGeneration_.load(
                           std::memory_order_relaxed)
                        == generation) {
                    emit owner->deviceStateChanged(deviceState);
                }
            },
            Qt::QueuedConnection);
    }

    void publishCommandResult(CommandResult result)
    {
        const QPointer<SerialTransportWorker> owner = owner_;
        const quint64 generation = generation_;
        QMetaObject::invokeMethod(
            owner_.data(),
            [owner, generation, result = std::move(result)] {
                if (owner != nullptr
                    && owner->commandGeneration_.load(
                           std::memory_order_relaxed)
                        == generation) {
                    emit owner->commandFinished(result);
                }
            },
            Qt::QueuedConnection);
    }

    void publishStatisticsIfDue(const qint64 now)
    {
        if (now - lastStatisticsMilliseconds_
            < kStatisticsIntervalMilliseconds) {
            return;
        }
        lastStatisticsMilliseconds_ = now;
        statistics_.replacedFrames =
            facadeReplacedFrames_ + outbound_.replacedFrameCount();
        const TransportStatistics snapshot = statistics_;
        const QPointer<SerialTransportWorker> owner = owner_;
        const quint64 generation = generation_;
        QMetaObject::invokeMethod(
            owner_.data(),
            [owner, generation, snapshot] {
                if (owner != nullptr
                    && owner->commandGeneration_.load(
                           std::memory_order_relaxed)
                        == generation) {
                    emit owner->statisticsChanged(snapshot);
                }
            },
            Qt::QueuedConnection);
    }

    QPointer<SerialTransportWorker> owner_;
    QTimer* timer_ = nullptr;
    QElapsedTimer monotonicClock_;
    ConnectionConfiguration configuration_;
    quint64 generation_{1};
    bool runningRequested_{};

    std::unique_ptr<NativeSerialPort> serial_;
    std::shared_ptr<AsynchronousOpenAttempt> openAttempt_;
    bool openAttemptTimedOut_{};
    qint64 openAttemptDeadlineMilliseconds_{};
    protocol::StreamDecoder decoder_;
    OutboundPacketQueue outbound_;
    QVector<NativePortRecord> candidates_;
    qsizetype candidateIndex_{};
    QString selectedPort_;
    detail::PortIdentitySnapshot selectedIdentity_;
    QString trustedPortNameHint_;
    QString trustedPersistentIdentity_;
    ConnectionState connectionState_{ConnectionState::Stopped};
    quint64 reconnectAttempt_{};
    qint64 nextConnectionAttemptMilliseconds_{};
    qint64 handshakeDeadlineMilliseconds_{};
    qint64 nextHandshakeAttemptMilliseconds_{};
    QByteArray handshakePacket_;
    qint64 lastValidPacketMilliseconds_{};
    qint64 lastStatePollMilliseconds_{};
    qint64 stateRequestDeadlineMilliseconds_{};
    qint64 lastStatisticsMilliseconds_{};
    quint32 sequence_{};
    quint32 helloSequence_{};
    detail::StateRequestSequenceWindow stateRequestSequences_;
    std::optional<quint32> lastDeviceUptimeMilliseconds_;
    bool explicitStateRequest_{};
    bool drainRequested_{};
    qint64 drainDeadlineMilliseconds_{};
    bool activeWriteWasFrame_{};
    bool ambientFramesEnabled_{};
    std::optional<protocol::OperatingMode> desiredMode_;
    std::optional<protocol::OperatingMode> desiredFallbackMode_;
    std::optional<QByteArray> pendingRgbFrame_;
    QByteArray lastRgbFrame_;
    qint64 lastFrameQueuedMilliseconds_{};
    quint64 frameEpoch_{1};
    int ambientTimeoutMilliseconds_{1'000};

    protocol::DeviceInfo deviceInformation_;
    DesiredControl stripControl_;
    DesiredControl brightnessControl_;
    DesiredControl effectControl_;
    DesiredControl fallbackModeControl_;
    DesiredControl modeControl_;
    std::unordered_map<quint32, PendingAcknowledgement>
        pendingAcknowledgements_;

    TransportStatistics statistics_;
    quint64 facadeReplacedFrames_{};
    ConnectionStatus lastStatus_;
    quint64 lastStatusGeneration_{};
};

SerialTransportWorker::SerialTransportWorker(QObject* parent)
    : QObject(parent),
      runtime_(new SerialTransportRuntime(this))
{
    qRegisterMetaType<PortSelectionMode>();
    qRegisterMetaType<ConnectionState>();
    qRegisterMetaType<SerialPortDescriptor>();
    qRegisterMetaType<QVector<SerialPortDescriptor>>();
    qRegisterMetaType<ConnectionConfiguration>();
    qRegisterMetaType<DeviceInformation>();
    qRegisterMetaType<DeviceState>();
    qRegisterMetaType<ConnectionStatus>();
    qRegisterMetaType<CommandResult>();
    qRegisterMetaType<TransportStatistics>();

    transportThread_.setObjectName(
        QStringLiteral("AmbilightSerialTransport"));
    runtime_->moveToThread(&transportThread_);
    connect(
        &transportThread_,
        &QThread::finished,
        runtime_,
        &QObject::deleteLater);
    transportThread_.start();
}

SerialTransportWorker::~SerialTransportWorker()
{
    runningRequested_.store(false, std::memory_order_relaxed);
    const quint64 generation =
        commandGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
    frameEpoch_.fetch_add(1, std::memory_order_relaxed);
    {
        const QMutexLocker locker(&pendingFrameMutex_);
        pendingFrame_.reset();
        frameDeliveryScheduled_ = false;
        ++frameDeliveryToken_;
    }
    if (runtime_ != nullptr && transportThread_.isRunning()) {
        QMetaObject::invokeMethod(
            runtime_,
            [runtime = runtime_, generation] {
                runtime->stopTransport(generation);
            },
            Qt::BlockingQueuedConnection);
    }
    transportThread_.quit();
    transportThread_.wait();
    runtime_ = nullptr;
}

QVector<SerialPortDescriptor> SerialTransportWorker::enumeratePorts(
    QString* errorMessage)
{
    return enumerateNativePorts(errorMessage);
}

bool SerialTransportWorker::requestPortRefresh()
{
    const QMutexLocker lifecycleLocker(&lifecycleMutex_);
    if (runtime_ == nullptr || !transportThread_.isRunning()) {
        return false;
    }
    return QMetaObject::invokeMethod(
        runtime_,
        [runtime = runtime_] {
            runtime->refreshPorts();
        },
        Qt::QueuedConnection);
}

void SerialTransportWorker::start()
{
    const QMutexLocker lifecycleLocker(&lifecycleMutex_);
    bool expected = false;
    if (runtime_ == nullptr
        || !transportThread_.isRunning()
        || !runningRequested_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_relaxed)) {
        return;
    }
    const quint64 generation =
        commandGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
    QMetaObject::invokeMethod(
        runtime_,
        [runtime = runtime_, generation] {
            runtime->startTransport(generation);
        },
        Qt::QueuedConnection);
}

void SerialTransportWorker::stop()
{
    const QMutexLocker lifecycleLocker(&lifecycleMutex_);
    if (runtime_ == nullptr || !transportThread_.isRunning()) {
        return;
    }
    runningRequested_.store(false, std::memory_order_relaxed);
    const quint64 generation =
        commandGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
    frameEpoch_.fetch_add(1, std::memory_order_relaxed);
    {
        const QMutexLocker locker(&pendingFrameMutex_);
        pendingFrame_.reset();
        frameDeliveryScheduled_ = false;
        ++frameDeliveryToken_;
    }
    if (QThread::currentThread() == &transportThread_) {
        runtime_->stopTransport(generation);
        return;
    }
    QMetaObject::invokeMethod(
        runtime_,
        [runtime = runtime_, generation] {
            runtime->stopTransport(generation);
        },
        Qt::BlockingQueuedConnection);
}

void SerialTransportWorker::updateConnectionConfiguration(
    const ConnectionConfiguration& configuration)
{
    if (runtime_ == nullptr || !transportThread_.isRunning()) {
        return;
    }
    const QMutexLocker lifecycleLocker(&lifecycleMutex_);
    ConnectionConfiguration normalizedConfiguration = configuration;
    normalizedConfiguration.portName =
        normalizedConfiguration.portName.trimmed().toUpper();
    if (normalizedConfiguration
        == requestedConnectionConfiguration_) {
        return;
    }
    requestedConnectionConfiguration_ = normalizedConfiguration;
    const quint64 generation =
        commandGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool shouldRun =
        runningRequested_.load(std::memory_order_relaxed);
    QMetaObject::invokeMethod(
        runtime_,
        [runtime = runtime_,
         configuration = std::move(normalizedConfiguration),
         generation,
         shouldRun] {
            runtime->updateConfiguration(
                configuration,
                generation,
                shouldRun);
        },
        Qt::QueuedConnection);
}

void SerialTransportWorker::sendStripConfiguration(
    const protocol::StripConfig& configuration)
{
    if (runtime_ == nullptr || !transportThread_.isRunning()) {
        return;
    }
    const quint64 frameEpoch =
        frameEpoch_.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        const QMutexLocker locker(&pendingFrameMutex_);
        pendingFrame_.reset();
        frameDeliveryScheduled_ = false;
        ++frameDeliveryToken_;
    }
    QMetaObject::invokeMethod(
        runtime_,
        [runtime = runtime_, configuration, frameEpoch] {
            runtime->setStripConfiguration(
                configuration,
                frameEpoch);
        },
        Qt::QueuedConnection);
}

void SerialTransportWorker::sendMode(
    const protocol::OperatingMode mode)
{
    if (runtime_ == nullptr || !transportThread_.isRunning()) {
        return;
    }
    quint64 frameEpoch =
        frameEpoch_.load(std::memory_order_relaxed);
    if (mode != protocol::OperatingMode::Ambilight) {
        frameEpoch =
            frameEpoch_.fetch_add(1, std::memory_order_relaxed) + 1;
        const QMutexLocker locker(&pendingFrameMutex_);
        pendingFrame_.reset();
        frameDeliveryScheduled_ = false;
        ++frameDeliveryToken_;
    }
    QMetaObject::invokeMethod(
        runtime_,
        [runtime = runtime_, mode, frameEpoch] {
            runtime->setMode(mode, frameEpoch);
        },
        Qt::QueuedConnection);
}

void SerialTransportWorker::sendFallbackMode(
    const protocol::OperatingMode mode)
{
    if (runtime_ == nullptr || !transportThread_.isRunning()) {
        return;
    }
    QMetaObject::invokeMethod(
        runtime_,
        [runtime = runtime_, mode] {
            runtime->setFallbackMode(mode);
        },
        Qt::QueuedConnection);
}

void SerialTransportWorker::sendBrightness(
    const protocol::Byte brightness)
{
    if (runtime_ == nullptr || !transportThread_.isRunning()) {
        return;
    }
    QMetaObject::invokeMethod(
        runtime_,
        [runtime = runtime_, brightness] {
            runtime->setBrightness(brightness);
        },
        Qt::QueuedConnection);
}

void SerialTransportWorker::sendEffect(
    const protocol::EffectConfig& effect)
{
    if (runtime_ == nullptr || !transportThread_.isRunning()) {
        return;
    }
    QMetaObject::invokeMethod(
        runtime_,
        [runtime = runtime_, effect] {
            runtime->setEffect(effect);
        },
        Qt::QueuedConnection);
}

void SerialTransportWorker::requestState()
{
    if (runtime_ == nullptr || !transportThread_.isRunning()) {
        return;
    }
    QMetaObject::invokeMethod(
        runtime_,
        [runtime = runtime_] {
            runtime->requestDeviceState();
        },
        Qt::QueuedConnection);
}

void SerialTransportWorker::requestDrain(const int timeoutMilliseconds)
{
    if (runtime_ == nullptr || !transportThread_.isRunning()) {
        emit drainFinished(false);
        return;
    }
    QMetaObject::invokeMethod(
        runtime_,
        [runtime = runtime_, timeoutMilliseconds] {
            runtime->requestDrain(timeoutMilliseconds);
        },
        Qt::QueuedConnection);
}

void SerialTransportWorker::submitLedFrame(const QByteArray& rgbPixels)
{
    if (runtime_ == nullptr
        || !transportThread_.isRunning()
        || !runningRequested_.load(std::memory_order_relaxed)) {
        return;
    }

    const quint64 generation =
        commandGeneration_.load(std::memory_order_relaxed);
    const quint64 frameEpoch =
        frameEpoch_.load(std::memory_order_relaxed);
    bool scheduleDelivery = false;
    quint64 deliveryToken = 0;
    {
        const QMutexLocker locker(&pendingFrameMutex_);
        if (pendingFrame_.has_value()) {
            ++facadeReplacedFrames_;
        }
        pendingFrame_ = PendingFrame{
            rgbPixels,
            generation,
            frameEpoch,
        };
        if (!frameDeliveryScheduled_) {
            frameDeliveryScheduled_ = true;
            deliveryToken = ++frameDeliveryToken_;
            scheduleDelivery = true;
        }
    }
    if (!scheduleDelivery) {
        return;
    }

    const QPointer<SerialTransportWorker> owner = this;
    QMetaObject::invokeMethod(
        runtime_,
        [owner, runtime = runtime_, deliveryToken] {
            if (owner == nullptr) {
                return;
            }

            std::optional<PendingFrame> frame;
            quint64 replacedFrames = 0;
            {
                const QMutexLocker locker(&owner->pendingFrameMutex_);
                if (deliveryToken != owner->frameDeliveryToken_) {
                    return;
                }
                frame = std::move(owner->pendingFrame_);
                owner->pendingFrame_.reset();
                owner->frameDeliveryScheduled_ = false;
                replacedFrames = owner->facadeReplacedFrames_;
            }
            if (frame.has_value()) {
                runtime->submitFrame(
                    frame->rgbPixels,
                    frame->generation,
                    frame->frameEpoch,
                    replacedFrames);
            }
        },
        Qt::QueuedConnection);
}

void SerialTransportWorker::clearPendingLedFrame()
{
    if (runtime_ == nullptr || !transportThread_.isRunning()) {
        return;
    }
    const quint64 frameEpoch =
        frameEpoch_.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        const QMutexLocker locker(&pendingFrameMutex_);
        pendingFrame_.reset();
        frameDeliveryScheduled_ = false;
        ++frameDeliveryToken_;
    }
    QMetaObject::invokeMethod(
        runtime_,
        [runtime = runtime_, frameEpoch] {
            runtime->clearFrame(frameEpoch);
        },
        Qt::QueuedConnection);
}

} // namespace ambilight::transport
