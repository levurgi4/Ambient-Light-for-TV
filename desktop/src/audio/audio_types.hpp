#pragma once

#include "audio_spectrum.hpp"

#include <QColor>
#include <QMetaType>
#include <QString>
#include <QVector>

namespace ambilight::audio {

enum class AudioSourceMode {
    WindowsOutput,
    Microphone,
    Mixed,
};

enum class AudioDeviceKind {
    Output,
    Input,
};

struct AudioDevice final {
    QString id;
    QString name;
    bool isDefault{};
};

struct AudioConfiguration final {
    AudioSourceMode sourceMode{AudioSourceMode::WindowsOutput};
    QString outputDeviceId;
    QString inputDeviceId;
    SpectrumAnalyzerConfiguration analyzer;
    SpectrumPerimeter perimeter;
    SpectrumLayout layout{SpectrumLayout::Mirrored};
    SpectrumColorMode colorMode{SpectrumColorMode::FrequencyGradient};
    SpectrumDynamics dynamics{SpectrumDynamics::Spring};
    QColor solidColor{0, 170, 255};
    int outputGainPercent{100};
    int microphoneGainPercent{100};
    int attackMilliseconds{25};
    int releaseMilliseconds{180};
    int springinessPercent{55};
    int targetFps{60};

    [[nodiscard]] bool isValid(QString* errorMessage = nullptr) const;
};

struct AudioFrame final {
    QVector<QRgb> colors;
    QVector<float> bandLevels;
    quint64 configurationGeneration{};
    quint64 frameNumber{};
    double framesPerSecond{};
    double peakLevel{};
};

enum class AudioState {
    Stopped,
    Starting,
    Running,
    Error,
};

struct AudioStatus final {
    AudioState state{AudioState::Stopped};
    QString message;
    QString outputDeviceName;
    QString inputDeviceName;
};

} // namespace ambilight::audio

Q_DECLARE_METATYPE(ambilight::audio::AudioDevice)
Q_DECLARE_METATYPE(QVector<ambilight::audio::AudioDevice>)
Q_DECLARE_METATYPE(ambilight::audio::AudioConfiguration)
Q_DECLARE_METATYPE(ambilight::audio::AudioFrame)
Q_DECLARE_METATYPE(ambilight::audio::AudioStatus)
