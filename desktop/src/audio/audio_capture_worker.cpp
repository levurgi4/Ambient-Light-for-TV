#include "audio_capture_worker.hpp"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QTimer>

#include <windows.h>

#include <audioclient.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace ambilight::audio {

using Microsoft::WRL::ComPtr;

namespace {

QString hresultMessage(const HRESULT result)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(result),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);
    QString message;
    if (length > 0 && buffer != nullptr) {
        message = QString::fromWCharArray(buffer, static_cast<qsizetype>(length))
                      .trimmed();
        LocalFree(buffer);
    }
    if (message.isEmpty()) {
        message = QStringLiteral("HRESULT 0x%1").arg(
            static_cast<qulonglong>(static_cast<unsigned long>(result)),
            8,
            16,
            QLatin1Char('0'));
    }
    return message;
}

class ComApartment final {
public:
    ComApartment()
    {
        result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ownsInitialization_ = result_ == S_OK || result_ == S_FALSE;
    }

    ~ComApartment()
    {
        if (ownsInitialization_) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool usable() const noexcept
    {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

    [[nodiscard]] HRESULT result() const noexcept
    {
        return result_;
    }

private:
    HRESULT result_{E_FAIL};
    bool ownsInitialization_{};
};

QString deviceId(IMMDevice* device)
{
    LPWSTR rawId = nullptr;
    if (device == nullptr || FAILED(device->GetId(&rawId)) || rawId == nullptr) {
        return {};
    }
    const QString result = QString::fromWCharArray(rawId);
    CoTaskMemFree(rawId);
    return result;
}

QString deviceName(IMMDevice* device)
{
    if (device == nullptr) {
        return {};
    }
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) {
        return deviceId(device);
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT result = properties->GetValue(PKEY_Device_FriendlyName, &value);
    QString name;
    if (SUCCEEDED(result) && value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        name = QString::fromWCharArray(value.pwszVal);
    }
    PropVariantClear(&value);
    return name.isEmpty() ? deviceId(device) : name;
}

EDataFlow dataFlow(const AudioDeviceKind kind)
{
    return kind == AudioDeviceKind::Output ? eRender : eCapture;
}

bool isFloatFormat(const WAVEFORMATEX& format)
{
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (format.wFormatTag != WAVE_FORMAT_EXTENSIBLE
        || format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return false;
    }
    const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
    return IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
}

bool isPcmFormat(const WAVEFORMATEX& format)
{
    if (format.wFormatTag == WAVE_FORMAT_PCM) {
        return true;
    }
    if (format.wFormatTag != WAVE_FORMAT_EXTENSIBLE
        || format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return false;
    }
    const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
    return IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
}

float readFloatSample(const BYTE* data, const int bytesPerSample)
{
    if (bytesPerSample == 4) {
        float value = 0.0F;
        std::memcpy(&value, data, sizeof(value));
        return std::clamp(value, -1.0F, 1.0F);
    }
    if (bytesPerSample == 8) {
        double value = 0.0;
        std::memcpy(&value, data, sizeof(value));
        return static_cast<float>(std::clamp(value, -1.0, 1.0));
    }
    return 0.0F;
}

float readPcmSample(const BYTE* data, const int bytesPerSample)
{
    switch (bytesPerSample) {
    case 1:
        return (static_cast<int>(*data) - 128) / 128.0F;
    case 2: {
        std::int16_t value{};
        std::memcpy(&value, data, sizeof(value));
        return static_cast<float>(value) / 32768.0F;
    }
    case 3: {
        std::int32_t value = static_cast<std::int32_t>(data[0])
            | (static_cast<std::int32_t>(data[1]) << 8)
            | (static_cast<std::int32_t>(data[2]) << 16);
        if ((value & 0x00800000) != 0) {
            value |= static_cast<std::int32_t>(0xFF000000);
        }
        return static_cast<float>(value) / 8388608.0F;
    }
    case 4: {
        std::int32_t value{};
        std::memcpy(&value, data, sizeof(value));
        return static_cast<float>(
            static_cast<double>(value) / 2147483648.0);
    }
    default:
        return 0.0F;
    }
}

class CaptureEndpoint final {
public:
    ~CaptureEndpoint()
    {
        reset();
    }

    CaptureEndpoint(const CaptureEndpoint&) = delete;
    CaptureEndpoint& operator=(const CaptureEndpoint&) = delete;
    CaptureEndpoint(CaptureEndpoint&&) = delete;
    CaptureEndpoint& operator=(CaptureEndpoint&&) = delete;

    CaptureEndpoint() = default;

    bool start(
        IMMDeviceEnumerator* enumerator,
        const EDataFlow flow,
        const QString& requestedDeviceId,
        const bool loopback,
        const SpectrumAnalyzerConfiguration& analyzerConfiguration,
        QString* errorMessage)
    {
        reset();
        HRESULT result = S_OK;
        if (requestedDeviceId.isEmpty()) {
            result = enumerator->GetDefaultAudioEndpoint(
                flow, eMultimedia, &device_);
        } else {
            result = enumerator->GetDevice(
                reinterpret_cast<LPCWSTR>(requestedDeviceId.utf16()),
                &device_);
        }
        if (FAILED(result)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Audio device is unavailable: %1")
                                    .arg(hresultMessage(result));
            }
            return false;
        }
        name_ = deviceName(device_.Get());

        result = device_->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            &audioClient_);
        if (FAILED(result)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Could not open %1: %2")
                                    .arg(name_, hresultMessage(result));
            }
            return false;
        }

        result = audioClient_->GetMixFormat(&format_);
        if (FAILED(result) || format_ == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Could not read the format of %1: %2")
                                    .arg(name_, hresultMessage(result));
            }
            return false;
        }
        if ((!isFloatFormat(*format_) && !isPcmFormat(*format_))
            || format_->nChannels == 0
            || format_->nBlockAlign == 0
            || format_->nSamplesPerSec < 8'000) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Unsupported audio format on %1.").arg(name_);
            }
            return false;
        }

        const DWORD flags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        result = audioClient_->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            flags,
            0,
            0,
            format_,
            nullptr);
        if (FAILED(result)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Could not initialize %1: %2")
                                    .arg(name_, hresultMessage(result));
            }
            return false;
        }

        result = audioClient_->GetService(
            __uuidof(IAudioCaptureClient),
            &captureClient_);
        if (FAILED(result)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Could not capture %1: %2")
                                    .arg(name_, hresultMessage(result));
            }
            return false;
        }
        analyzer_.setConfiguration(analyzerConfiguration);
        result = audioClient_->Start();
        if (FAILED(result)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Could not start %1: %2")
                                    .arg(name_, hresultMessage(result));
            }
            return false;
        }
        started_ = true;
        return true;
    }

    void reset()
    {
        if (started_ && audioClient_ != nullptr) {
            audioClient_->Stop();
        }
        started_ = false;
        captureClient_.Reset();
        audioClient_.Reset();
        device_.Reset();
        if (format_ != nullptr) {
            CoTaskMemFree(format_);
            format_ = nullptr;
        }
        name_.clear();
        analyzer_.reset();
    }

    [[nodiscard]] bool drain(QString* errorMessage)
    {
        if (!started_ || captureClient_ == nullptr || format_ == nullptr) {
            return false;
        }
        while (true) {
            UINT32 packetFrames = 0;
            HRESULT result = captureClient_->GetNextPacketSize(&packetFrames);
            if (FAILED(result)) {
                if (errorMessage != nullptr) {
                    *errorMessage = QStringLiteral("Audio stream failed on %1: %2")
                                        .arg(name_, hresultMessage(result));
                }
                return false;
            }
            if (packetFrames == 0) {
                return true;
            }

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            result = captureClient_->GetBuffer(
                &data, &frames, &flags, nullptr, nullptr);
            if (FAILED(result)) {
                if (errorMessage != nullptr) {
                    *errorMessage = QStringLiteral("Could not read %1: %2")
                                        .arg(name_, hresultMessage(result));
                }
                return false;
            }

            std::vector<float> mono(frames, 0.0F);
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data != nullptr) {
                const int channels = format_->nChannels;
                const int bytesPerSample = format_->nBlockAlign / channels;
                const bool floatingPoint = isFloatFormat(*format_);
                for (UINT32 frame = 0; frame < frames; ++frame) {
                    float sum = 0.0F;
                    for (int channel = 0; channel < channels; ++channel) {
                        const BYTE* sample = data
                            + static_cast<std::size_t>(frame) * format_->nBlockAlign
                            + static_cast<std::size_t>(channel) * bytesPerSample;
                        sum += floatingPoint
                            ? readFloatSample(sample, bytesPerSample)
                            : readPcmSample(sample, bytesPerSample);
                    }
                    mono[frame] = std::clamp(sum / channels, -1.0F, 1.0F);
                }
            }
            static_cast<void>(analyzer_.pushSamples(
                mono,
                static_cast<int>(format_->nSamplesPerSec)));
            result = captureClient_->ReleaseBuffer(frames);
            if (FAILED(result)) {
                if (errorMessage != nullptr) {
                    *errorMessage = QStringLiteral("Could not release the audio buffer for %1: %2")
                                        .arg(name_, hresultMessage(result));
                }
                return false;
            }
        }
    }

    [[nodiscard]] const std::vector<float>& levels() const noexcept
    {
        return analyzer_.levels();
    }

    [[nodiscard]] const QString& name() const noexcept
    {
        return name_;
    }

private:
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> audioClient_;
    ComPtr<IAudioCaptureClient> captureClient_;
    WAVEFORMATEX* format_ = nullptr;
    QString name_;
    AudioSpectrumAnalyzer analyzer_;
    bool started_{};
};

} // namespace

bool AudioConfiguration::isValid(QString* errorMessage) const
{
    const bool valid = analyzer.isValid()
        && perimeter.isValid() && perimeter.total() <= 300
        && solidColor.isValid()
        && outputGainPercent >= 0 && outputGainPercent <= 200
        && microphoneGainPercent >= 0 && microphoneGainPercent <= 200
        && attackMilliseconds >= 0 && attackMilliseconds <= 500
        && releaseMilliseconds >= 10 && releaseMilliseconds <= 2'000
        && springinessPercent >= 0 && springinessPercent <= 100
        && targetFps >= 10 && targetFps <= 120;
    if (!valid && errorMessage != nullptr) {
        *errorMessage = QStringLiteral("The audio visualizer configuration is invalid.");
    }
    return valid;
}

class AudioCaptureRuntime final : public QObject {
public:
    explicit AudioCaptureRuntime(AudioCaptureWorker* owner)
        : owner_(owner)
    {
    }

    void start()
    {
        if (wantedRunning_) {
            return;
        }
        wantedRunning_ = true;
        ensureTimer();
        restart();
    }

    void stop()
    {
        wantedRunning_ = false;
        running_ = false;
        outputEndpoint_.reset();
        inputEndpoint_.reset();
        smoother_.reset();
        if (timer_ != nullptr) {
            timer_->stop();
        }
        publishStatus(AudioState::Stopped, QStringLiteral("Audio capture stopped."));
    }

    void updateConfiguration(
        AudioConfiguration configuration,
        const quint64 generation)
    {
        configuration_ = std::move(configuration);
        generation_ = generation;
        smoother_.reset();
        if (wantedRunning_) {
            ensureTimer();
            restart();
        }
    }

    void shutdown()
    {
        stop();
        timer_.reset();
        apartment_.reset();
    }

private:
    void ensureTimer()
    {
        if (timer_ != nullptr) {
            return;
        }
        apartment_ = std::make_unique<ComApartment>();
        timer_ = std::make_unique<QTimer>();
        timer_->setTimerType(Qt::PreciseTimer);
        QObject::connect(timer_.get(), &QTimer::timeout, [this] { poll(); });
        outputClock_.start();
    }

    void restart()
    {
        running_ = false;
        outputEndpoint_.reset();
        inputEndpoint_.reset();
        smoother_.reset();
        if (!wantedRunning_) {
            return;
        }
        publishStatus(AudioState::Starting, QStringLiteral("Starting audio capture."));

        QString error;
        if (!configuration_.isValid(&error)) {
            fail(error);
            return;
        }
        if (apartment_ == nullptr || !apartment_->usable()) {
            fail(QStringLiteral("COM initialization failed: %1")
                     .arg(hresultMessage(
                         apartment_ == nullptr ? E_UNEXPECTED : apartment_->result())));
            return;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        const HRESULT result = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator));
        if (FAILED(result)) {
            fail(QStringLiteral("Could not enumerate audio devices: %1")
                     .arg(hresultMessage(result)));
            return;
        }

        const bool useOutput = configuration_.sourceMode
                == AudioSourceMode::WindowsOutput
            || configuration_.sourceMode == AudioSourceMode::Mixed;
        const bool useInput = configuration_.sourceMode
                == AudioSourceMode::Microphone
            || configuration_.sourceMode == AudioSourceMode::Mixed;
        if (useOutput
            && !outputEndpoint_.start(
                enumerator.Get(),
                eRender,
                configuration_.outputDeviceId,
                true,
                configuration_.analyzer,
                &error)) {
            fail(error);
            return;
        }
        if (useInput
            && !inputEndpoint_.start(
                enumerator.Get(),
                eCapture,
                configuration_.inputDeviceId,
                false,
                configuration_.analyzer,
                &error)) {
            fail(error);
            return;
        }

        running_ = true;
        frameNumber_ = 0;
        outputClock_.restart();
        timer_->setInterval(5);
        timer_->start();
        publishStatus(AudioState::Running, QStringLiteral("Audio capture active."));
    }

    void fail(const QString& message)
    {
        running_ = false;
        outputEndpoint_.reset();
        inputEndpoint_.reset();
        publishStatus(AudioState::Error, message);
        if (timer_ != nullptr && wantedRunning_) {
            timer_->setInterval(1'000);
            timer_->start();
        }
    }

    void poll()
    {
        if (!wantedRunning_) {
            return;
        }
        if (!running_) {
            restart();
            return;
        }

        QString error;
        const bool useOutput = configuration_.sourceMode
                == AudioSourceMode::WindowsOutput
            || configuration_.sourceMode == AudioSourceMode::Mixed;
        const bool useInput = configuration_.sourceMode
                == AudioSourceMode::Microphone
            || configuration_.sourceMode == AudioSourceMode::Mixed;
        if (useOutput && !outputEndpoint_.drain(&error)) {
            fail(error);
            return;
        }
        if (useInput && !inputEndpoint_.drain(&error)) {
            fail(error);
            return;
        }

        const qint64 frameInterval = std::max<qint64>(
            1, 1'000 / configuration_.targetFps);
        const qint64 elapsed = outputClock_.elapsed();
        if (elapsed < frameInterval) {
            return;
        }
        outputClock_.restart();

        std::vector<float> combined(
            static_cast<std::size_t>(configuration_.analyzer.bandCount),
            0.0F);
        const auto mixIn = [&combined](
                               const std::vector<float>& source,
                               const float gain,
                               const bool combine) {
            const std::size_t count = std::min(combined.size(), source.size());
            for (std::size_t index = 0; index < count; ++index) {
                const float level = std::clamp(source[index] * gain, 0.0F, 1.0F);
                combined[index] = combine
                    ? 1.0F - ((1.0F - combined[index]) * (1.0F - level))
                    : level;
            }
        };
        if (useOutput) {
            mixIn(
                outputEndpoint_.levels(),
                configuration_.outputGainPercent / 100.0F,
                false);
        }
        if (useInput) {
            mixIn(
                inputEndpoint_.levels(),
                configuration_.microphoneGainPercent / 100.0F,
                useOutput);
        }

        const auto smoothed = smoother_.apply(
            combined,
            static_cast<double>(elapsed),
            configuration_.attackMilliseconds,
            configuration_.releaseMilliseconds,
            configuration_.dynamics,
            configuration_.springinessPercent);
        const auto colors = makeSpectrumLedColors(
            smoothed,
            configuration_.perimeter,
            configuration_.layout,
            configuration_.colorMode,
            configuration_.solidColor);
        if (colors.isEmpty()) {
            return;
        }

        AudioFrame frame;
        frame.colors.reserve(colors.size());
        for (const QColor& color : colors) {
            frame.colors.push_back(color.rgb());
        }
        frame.bandLevels.reserve(static_cast<qsizetype>(smoothed.size()));
        for (const float level : smoothed) {
            frame.bandLevels.push_back(level);
        }
        frame.configurationGeneration = generation_;
        frame.frameNumber = ++frameNumber_;
        frame.framesPerSecond = elapsed > 0
            ? 1'000.0 / static_cast<double>(elapsed)
            : 0.0;
        frame.peakLevel = smoothed.empty()
            ? 0.0
            : std::clamp(
                static_cast<double>(
                    *std::max_element(smoothed.begin(), smoothed.end())),
                0.0,
                1.0);
        emit owner_->frameReady(frame);
    }

    void publishStatus(const AudioState state, const QString& message)
    {
        AudioStatus status;
        status.state = state;
        status.message = message;
        status.outputDeviceName = outputEndpoint_.name();
        status.inputDeviceName = inputEndpoint_.name();
        emit owner_->statusChanged(status);
    }

    AudioCaptureWorker* owner_ = nullptr;
    std::unique_ptr<ComApartment> apartment_;
    std::unique_ptr<QTimer> timer_;
    AudioConfiguration configuration_;
    CaptureEndpoint outputEndpoint_;
    CaptureEndpoint inputEndpoint_;
    SpectrumSmoother smoother_;
    QElapsedTimer outputClock_;
    quint64 generation_{};
    quint64 frameNumber_{};
    bool wantedRunning_{};
    bool running_{};
};

AudioCaptureWorker::AudioCaptureWorker(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<AudioConfiguration>();
    qRegisterMetaType<AudioFrame>();
    qRegisterMetaType<AudioStatus>();
    runtime_ = new AudioCaptureRuntime(this);
    runtime_->moveToThread(&audioThread_);
    connect(&audioThread_, &QThread::finished, runtime_, &QObject::deleteLater);
    audioThread_.setObjectName(QStringLiteral("Ambilight audio capture"));
    audioThread_.start();
}

AudioCaptureWorker::~AudioCaptureWorker()
{
    if (runtime_ != nullptr && audioThread_.isRunning()) {
        QMetaObject::invokeMethod(
            runtime_,
            [runtime = runtime_] { runtime->shutdown(); },
            Qt::BlockingQueuedConnection);
    }
    audioThread_.quit();
    audioThread_.wait();
    runtime_ = nullptr;
}

QVector<AudioDevice> AudioCaptureWorker::enumerateDevices(
    const AudioDeviceKind kind,
    QString* errorMessage)
{
    ComApartment apartment;
    if (!apartment.usable()) {
        if (errorMessage != nullptr) {
            *errorMessage = hresultMessage(apartment.result());
        }
        return {};
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) {
        if (errorMessage != nullptr) {
            *errorMessage = hresultMessage(result);
        }
        return {};
    }

    QString defaultId;
    ComPtr<IMMDevice> defaultDevice;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(
            dataFlow(kind), eMultimedia, &defaultDevice))) {
        defaultId = deviceId(defaultDevice.Get());
    }

    ComPtr<IMMDeviceCollection> collection;
    result = enumerator->EnumAudioEndpoints(
        dataFlow(kind), DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(result)) {
        if (errorMessage != nullptr) {
            *errorMessage = hresultMessage(result);
        }
        return {};
    }

    UINT count = 0;
    collection->GetCount(&count);
    QVector<AudioDevice> devices;
    devices.reserve(static_cast<qsizetype>(count));
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, &device))) {
            continue;
        }
        AudioDevice descriptor;
        descriptor.id = deviceId(device.Get());
        descriptor.name = deviceName(device.Get());
        descriptor.isDefault = descriptor.id == defaultId;
        if (!descriptor.id.isEmpty()) {
            devices.push_back(std::move(descriptor));
        }
    }
    std::sort(
        devices.begin(),
        devices.end(),
        [](const AudioDevice& first, const AudioDevice& second) {
            if (first.isDefault != second.isDefault) {
                return first.isDefault;
            }
            return QString::localeAwareCompare(first.name, second.name) < 0;
        });
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return devices;
}

void AudioCaptureWorker::start()
{
    if (runtime_ == nullptr) {
        return;
    }
    QMetaObject::invokeMethod(
        runtime_, [runtime = runtime_] { runtime->start(); }, Qt::QueuedConnection);
}

void AudioCaptureWorker::stop()
{
    if (runtime_ == nullptr) {
        return;
    }
    QMetaObject::invokeMethod(
        runtime_, [runtime = runtime_] { runtime->stop(); }, Qt::QueuedConnection);
}

void AudioCaptureWorker::updateConfiguration(
    const AudioConfiguration& configuration)
{
    if (runtime_ == nullptr) {
        return;
    }
    const quint64 generation =
        configurationGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
    QMetaObject::invokeMethod(
        runtime_,
        [runtime = runtime_, configuration, generation] {
            runtime->updateConfiguration(configuration, generation);
        },
        Qt::QueuedConnection);
}

} // namespace ambilight::audio
