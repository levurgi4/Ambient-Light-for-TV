#pragma once

#include "audio_types.hpp"

#include <QObject>
#include <QThread>

#include <atomic>

namespace ambilight::audio {

class AudioCaptureRuntime;

// Public facade in the UI thread. WASAPI, COM and FFT processing stay on the
// dedicated audio thread owned by this object.
class AudioCaptureWorker final : public QObject {
    Q_OBJECT

public:
    explicit AudioCaptureWorker(QObject* parent = nullptr);
    ~AudioCaptureWorker() override;

    AudioCaptureWorker(const AudioCaptureWorker&) = delete;
    AudioCaptureWorker& operator=(const AudioCaptureWorker&) = delete;
    AudioCaptureWorker(AudioCaptureWorker&&) = delete;
    AudioCaptureWorker& operator=(AudioCaptureWorker&&) = delete;

    [[nodiscard]] static QVector<AudioDevice> enumerateDevices(
        AudioDeviceKind kind,
        QString* errorMessage = nullptr);

public slots:
    void start();
    void stop();
    void updateConfiguration(
        const ambilight::audio::AudioConfiguration& configuration);

signals:
    void frameReady(const ambilight::audio::AudioFrame& frame);
    void statusChanged(const ambilight::audio::AudioStatus& status);

private:
    friend class AudioCaptureRuntime;

    QThread audioThread_;
    AudioCaptureRuntime* runtime_ = nullptr;
    std::atomic<quint64> configurationGeneration_{1};
};

} // namespace ambilight::audio
