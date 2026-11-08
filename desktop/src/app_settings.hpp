#pragma once

#include <QSettings>
#include <QString>

namespace ambilight {

struct AppSettings final {
    QString language = QStringLiteral("ru");
    bool lightingEnabled = true;
    bool startWithWindows = false;
    bool closeToTray = true;
    QString hotkey = QStringLiteral("Ctrl+Alt+A");

    int screenWidth = 3840;
    int screenHeight = 2160;
    QString captureOutputId;
    QString captureMethod = QStringLiteral("dxgi");
    QString mirrorOutputId;
    int mirrorFps = 60;
    int mirrorFillMode = 0;
    int mirrorBlurPercent = 60;
    int mirrorBackgroundBrightness = 70;
    int mirrorContentFormat = 0;
    int targetFps = 60;
    bool gamingMode = false;
    int samplingZones = 300;
    bool automaticSamplingDepth = true;
    int samplingDepthPixels = 120;
    QString regionColorMode = QStringLiteral("mean");
    QString ledTransferMode = QStringLiteral("srgb");
    QString blackBarDetectionMode = QStringLiteral("first-content");
    int smoothingMilliseconds = 25;
    bool flashProtectionEnabled = true;

    QString ledType = QStringLiteral("WS2815");
    QString colorOrder = QStringLiteral("RGB");
    int topLeds = 96;
    int rightLeds = 54;
    int bottomLeds = 96;
    int leftLeds = 54;
    int ledOffset = 0;
    bool clockwise = true;
    int brightnessPercent = 50;
    int currentLimitMilliAmps = 5000;
    int redCalibrationPercent = 100;
    int greenCalibrationPercent = 100;
    int blueCalibrationPercent = 100;

    QString activeMode = QStringLiteral("ambilight");
    QString standaloneEffect = QStringLiteral("rainbow");
    QString solidColor = QStringLiteral("#ff6018");

    QString audioSource = QStringLiteral("windows");
    QString audioOutputDeviceId;
    QString audioInputDeviceId;
    int audioBandCount = 24;
    int audioMinimumFrequencyHz = 40;
    int audioMaximumFrequencyHz = 16'000;
    int audioSensitivityPercent = 100;
    int audioNoiseGateDecibels = -55;
    int audioOutputGainPercent = 100;
    int audioMicrophoneGainPercent = 100;
    int audioAttackMilliseconds = 25;
    int audioReleaseMilliseconds = 180;
    QString audioDynamics = QStringLiteral("spring");
    int audioSpringinessPercent = 55;
    QString audioLayout = QStringLiteral("mirrored");
    QString audioColorMode = QStringLiteral("spectrum");
    QString audioSolidColor = QStringLiteral("#00aaff");

    bool automaticPortSelection = true;
    QString portName = QStringLiteral("COM3");
    int baudRate = 2'000'000;
    int ambientTimeoutMs = 1000;

    [[nodiscard]] int ledCount() const noexcept;
    [[nodiscard]] static int minimumCurrentLimitMilliAmps(
        int ledCount) noexcept;
    void normalize();

    [[nodiscard]] static AppSettings load(QSettings& settings);
    [[nodiscard]] bool save(QSettings& settings) const;
};

} // namespace ambilight
