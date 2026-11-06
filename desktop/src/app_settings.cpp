#include "app_settings.hpp"

#include <QColor>

#include <algorithm>
#include <array>
#include <iterator>

namespace ambilight {

namespace {

template<typename T>
T valueOr(QSettings& settings, const char* key, const T& fallback)
{
    return settings.value(QLatin1String(key), QVariant::fromValue(fallback)).template value<T>();
}

} // namespace

int AppSettings::ledCount() const noexcept
{
    return topLeds + rightLeds + bottomLeds + leftLeds;
}

int AppSettings::minimumCurrentLimitMilliAmps(
    const int ledCount) noexcept
{
    constexpr int idleCurrentPerLedMilliAmps = 3;
    return std::max(100, std::max(0, ledCount) * idleCurrentPerLedMilliAmps);
}

void AppSettings::normalize()
{
    if (language != QStringLiteral("ru") && language != QStringLiteral("en")) {
        language = QStringLiteral("ru");
    }
    if (ledType != QStringLiteral("WS2815")) {
        ledType = QStringLiteral("WS2815");
    }
    if (colorOrder != QStringLiteral("RGB")
        && colorOrder != QStringLiteral("RBG")
        && colorOrder != QStringLiteral("GRB")
        && colorOrder != QStringLiteral("GBR")
        && colorOrder != QStringLiteral("BRG")
        && colorOrder != QStringLiteral("BGR")) {
        colorOrder = QStringLiteral("RGB");
    }
    if (activeMode != QStringLiteral("ambilight")
        && activeMode != QStringLiteral("equalizer")
        && activeMode != QStringLiteral("solid")
        && activeMode != QStringLiteral("breathing")
        && activeMode != QStringLiteral("rainbow")
        && activeMode != QStringLiteral("wave")
        && activeMode != QStringLiteral("off")) {
        activeMode = QStringLiteral("ambilight");
    }
    if (standaloneEffect != QStringLiteral("solid")
        && standaloneEffect != QStringLiteral("breathing")
        && standaloneEffect != QStringLiteral("rainbow")
        && standaloneEffect != QStringLiteral("wave")
        && standaloneEffect != QStringLiteral("off")) {
        standaloneEffect = QStringLiteral("rainbow");
    }
    if (!QColor(solidColor).isValid()) {
        solidColor = QStringLiteral("#ff6018");
    } else {
        solidColor = QColor(solidColor).name(QColor::HexRgb);
    }
    hotkey = hotkey.trimmed();
    captureOutputId = captureOutputId.trimmed();
    mirrorOutputId = mirrorOutputId.trimmed();
    mirrorFps = std::clamp(mirrorFps, 15, 60);
    audioOutputDeviceId = audioOutputDeviceId.trimmed();
    audioInputDeviceId = audioInputDeviceId.trimmed();

    if (audioSource != QStringLiteral("windows")
        && audioSource != QStringLiteral("microphone")
        && audioSource != QStringLiteral("mixed")) {
        audioSource = QStringLiteral("windows");
    }
    if (audioLayout != QStringLiteral("mirrored")
        && audioLayout != QStringLiteral("sequential")) {
        audioLayout = QStringLiteral("mirrored");
    }
    if (audioColorMode != QStringLiteral("spectrum")
        && audioColorMode != QStringLiteral("solid")) {
        audioColorMode = QStringLiteral("spectrum");
    }
    if (audioDynamics != QStringLiteral("smooth")
        && audioDynamics != QStringLiteral("spring")) {
        audioDynamics = QStringLiteral("spring");
    }
    if (!QColor(audioSolidColor).isValid()) {
        audioSolidColor = QStringLiteral("#00aaff");
    } else {
        audioSolidColor = QColor(audioSolidColor).name(QColor::HexRgb);
    }

    screenWidth = std::clamp(screenWidth, 320, 16'384);
    screenHeight = std::clamp(screenHeight, 240, 16'384);
    targetFps = std::clamp(targetFps, 1, 240);
    if (captureMethod != QStringLiteral("dxgi")
        && captureMethod != QStringLiteral("wgc")) {
        captureMethod = QStringLiteral("dxgi");
    }
    samplingZones = std::clamp(samplingZones, 4, 300);
    samplingDepthPixels = std::clamp(samplingDepthPixels, 8, 512);
    constexpr std::array supportedSmoothing{0, 25, 60, 120};
    if (std::find(
            supportedSmoothing.begin(),
            supportedSmoothing.end(),
            smoothingMilliseconds)
        == supportedSmoothing.end()) {
        smoothingMilliseconds = 25;
    }
    if (regionColorMode != QStringLiteral("mean")
        && regionColorMode != QStringLiteral("rms")
        && regionColorMode != QStringLiteral("dominant")) {
        regionColorMode = QStringLiteral("mean");
    }
    if (ledTransferMode != QStringLiteral("raw")
        && ledTransferMode != QStringLiteral("soft-gamma")
        && ledTransferMode != QStringLiteral("srgb")
        && ledTransferMode != QStringLiteral("strong-gamma")) {
        ledTransferMode = QStringLiteral("srgb");
    }
    if (blackBarDetectionMode != QStringLiteral("off")
        && blackBarDetectionMode != QStringLiteral("first-content")
        && blackBarDetectionMode != QStringLiteral("limit-21-9")) {
        blackBarDetectionMode = QStringLiteral("first-content");
    }

    topLeds = std::clamp(topLeds, 0, 2'000);
    rightLeds = std::clamp(rightLeds, 0, 2'000);
    bottomLeds = std::clamp(bottomLeds, 0, 2'000);
    leftLeds = std::clamp(leftLeds, 0, 2'000);

    if (ledCount() == 0) {
        topLeds = 96;
        rightLeds = 54;
        bottomLeds = 96;
        leftLeds = 54;
    }

    constexpr int maximumLedCount = 300;
    const int total = ledCount();
    if (total > maximumLedCount) {
        const std::array<int, 4> original{
            topLeds, rightLeds, bottomLeds, leftLeds};
        std::array<int, 4> scaled{};
        std::array<int, 4> remainders{};
        int assigned = 0;
        for (std::size_t index = 0; index < original.size(); ++index) {
            const int product = original[index] * maximumLedCount;
            scaled[index] = product / total;
            remainders[index] = product % total;
            assigned += scaled[index];
        }
        while (assigned < maximumLedCount) {
            const auto best = static_cast<std::size_t>(
                std::distance(
                    remainders.begin(),
                    std::max_element(
                        remainders.begin(),
                        remainders.end())));
            ++scaled[best];
            remainders[best] = -1;
            ++assigned;
        }
        topLeds = scaled[0];
        rightLeds = scaled[1];
        bottomLeds = scaled[2];
        leftLeds = scaled[3];
    }

    ledOffset = std::clamp(ledOffset, 0, std::max(0, ledCount() - 1));
    brightnessPercent = std::clamp(brightnessPercent, 0, 100);
    redCalibrationPercent = std::clamp(redCalibrationPercent, 0, 100);
    greenCalibrationPercent = std::clamp(greenCalibrationPercent, 0, 100);
    blueCalibrationPercent = std::clamp(blueCalibrationPercent, 0, 100);
    audioBandCount = std::clamp(audioBandCount, 4, 64);
    audioMinimumFrequencyHz = std::clamp(
        audioMinimumFrequencyHz, 20, 2'000);
    audioMaximumFrequencyHz = std::clamp(
        audioMaximumFrequencyHz, 1'000, 24'000);
    if (audioMaximumFrequencyHz <= audioMinimumFrequencyHz) {
        audioMinimumFrequencyHz = 40;
        audioMaximumFrequencyHz = 16'000;
    }
    audioSensitivityPercent = std::clamp(audioSensitivityPercent, 10, 400);
    audioNoiseGateDecibels = std::clamp(audioNoiseGateDecibels, -100, -10);
    audioOutputGainPercent = std::clamp(audioOutputGainPercent, 0, 200);
    audioMicrophoneGainPercent = std::clamp(
        audioMicrophoneGainPercent, 0, 200);
    audioAttackMilliseconds = std::clamp(audioAttackMilliseconds, 0, 500);
    audioReleaseMilliseconds = std::clamp(
        audioReleaseMilliseconds, 10, 2'000);
    audioSpringinessPercent = std::clamp(
        audioSpringinessPercent, 0, 100);
    currentLimitMilliAmps = std::clamp(
        currentLimitMilliAmps,
        minimumCurrentLimitMilliAmps(ledCount()),
        10'000);
    // The current ESP32 firmware UART is intentionally fixed at 2 Mbaud.
    // Normalize older experimental values so the desktop cannot enter a
    // reconnect loop with an unsupported rate.
    baudRate = 2'000'000;
    ambientTimeoutMs = std::clamp(ambientTimeoutMs, 100, 60'000);

    portName = portName.trimmed().toUpper();
    if (!automaticPortSelection && portName.isEmpty()) {
        portName = QStringLiteral("COM3");
    }
}

AppSettings AppSettings::load(QSettings& settings)
{
    AppSettings result;

    settings.beginGroup(QStringLiteral("general"));
    result.language = valueOr(settings, "language", result.language);
    result.lightingEnabled = valueOr(settings, "lightingEnabled", result.lightingEnabled);
    result.startWithWindows = valueOr(settings, "startWithWindows", result.startWithWindows);
    result.closeToTray = valueOr(settings, "closeToTray", result.closeToTray);
    result.hotkey = valueOr(settings, "hotkey", result.hotkey);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("screen"));
    result.screenWidth = valueOr(settings, "width", result.screenWidth);
    result.screenHeight = valueOr(settings, "height", result.screenHeight);
    result.captureOutputId = valueOr(settings, "outputId", result.captureOutputId);
    result.captureMethod = valueOr(settings, "captureMethod", result.captureMethod);
    result.mirrorOutputId = valueOr(settings, "mirrorOutputId", result.mirrorOutputId);
    result.mirrorFps = valueOr(settings, "mirrorFps", result.mirrorFps);
    result.targetFps = valueOr(settings, "targetFps", result.targetFps);
    result.gamingMode = valueOr(settings, "gamingMode", result.gamingMode);
    result.samplingZones = valueOr(settings, "samplingZones", result.samplingZones);
    result.automaticSamplingDepth = valueOr(
        settings,
        "automaticSamplingDepth",
        result.automaticSamplingDepth);
    result.samplingDepthPixels = valueOr(
        settings,
        "samplingDepthPixels",
        result.samplingDepthPixels);
    result.regionColorMode = valueOr(
        settings,
        "regionColorMode",
        result.regionColorMode);
    result.ledTransferMode = valueOr(
        settings,
        "ledTransferMode",
        result.ledTransferMode);
    if (settings.contains(QStringLiteral("blackBarDetectionMode"))) {
        result.blackBarDetectionMode = valueOr(
            settings,
            "blackBarDetectionMode",
            result.blackBarDetectionMode);
    } else {
        // Migrate the checkbox used through v0.2.4 without changing the
        // user's existing behavior.
        result.blackBarDetectionMode = valueOr(
            settings,
            "blackBarAutodetect",
            true)
            ? QStringLiteral("first-content")
            : QStringLiteral("off");
    }
    result.smoothingMilliseconds = valueOr(
        settings,
        "smoothingMilliseconds",
        result.smoothingMilliseconds);
    result.flashProtectionEnabled = valueOr(
        settings,
        "flashProtectionEnabled",
        result.flashProtectionEnabled);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("ledStrip"));
    result.ledType = valueOr(settings, "type", result.ledType);
    result.colorOrder = valueOr(settings, "colorOrder", result.colorOrder);
    result.topLeds = valueOr(settings, "top", result.topLeds);
    result.rightLeds = valueOr(settings, "right", result.rightLeds);
    result.bottomLeds = valueOr(settings, "bottom", result.bottomLeds);
    result.leftLeds = valueOr(settings, "left", result.leftLeds);
    result.ledOffset = valueOr(settings, "offset", result.ledOffset);
    result.clockwise = valueOr(settings, "clockwise", result.clockwise);
    result.brightnessPercent = valueOr(settings, "brightnessPercent", result.brightnessPercent);
    result.currentLimitMilliAmps =
        valueOr(settings, "currentLimitMilliAmps", result.currentLimitMilliAmps);
    result.redCalibrationPercent =
        valueOr(settings, "redCalibrationPercent", result.redCalibrationPercent);
    result.greenCalibrationPercent =
        valueOr(settings, "greenCalibrationPercent", result.greenCalibrationPercent);
    result.blueCalibrationPercent =
        valueOr(settings, "blueCalibrationPercent", result.blueCalibrationPercent);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("effects"));
    result.activeMode = valueOr(settings, "activeMode", result.activeMode);
    result.standaloneEffect = valueOr(settings, "standaloneEffect", result.standaloneEffect);
    result.solidColor = valueOr(settings, "solidColor", result.solidColor);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("audio"));
    result.audioSource = valueOr(settings, "source", result.audioSource);
    result.audioOutputDeviceId = valueOr(
        settings, "outputDeviceId", result.audioOutputDeviceId);
    result.audioInputDeviceId = valueOr(
        settings, "inputDeviceId", result.audioInputDeviceId);
    result.audioBandCount = valueOr(settings, "bandCount", result.audioBandCount);
    result.audioMinimumFrequencyHz = valueOr(
        settings, "minimumFrequencyHz", result.audioMinimumFrequencyHz);
    result.audioMaximumFrequencyHz = valueOr(
        settings, "maximumFrequencyHz", result.audioMaximumFrequencyHz);
    result.audioSensitivityPercent = valueOr(
        settings, "sensitivityPercent", result.audioSensitivityPercent);
    result.audioNoiseGateDecibels = valueOr(
        settings, "noiseGateDecibels", result.audioNoiseGateDecibels);
    result.audioOutputGainPercent = valueOr(
        settings, "outputGainPercent", result.audioOutputGainPercent);
    result.audioMicrophoneGainPercent = valueOr(
        settings, "microphoneGainPercent", result.audioMicrophoneGainPercent);
    result.audioAttackMilliseconds = valueOr(
        settings, "attackMilliseconds", result.audioAttackMilliseconds);
    result.audioReleaseMilliseconds = valueOr(
        settings, "releaseMilliseconds", result.audioReleaseMilliseconds);
    result.audioDynamics = valueOr(
        settings, "dynamics", result.audioDynamics);
    result.audioSpringinessPercent = valueOr(
        settings, "springinessPercent", result.audioSpringinessPercent);
    result.audioLayout = valueOr(settings, "layout", result.audioLayout);
    result.audioColorMode = valueOr(settings, "colorMode", result.audioColorMode);
    result.audioSolidColor = valueOr(settings, "solidColor", result.audioSolidColor);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("connection"));
    result.automaticPortSelection =
        valueOr(settings, "automaticPortSelection", result.automaticPortSelection);
    result.portName = valueOr(settings, "portName", result.portName);
    result.baudRate = valueOr(settings, "baudRate", result.baudRate);
    result.ambientTimeoutMs = valueOr(settings, "ambientTimeoutMs", result.ambientTimeoutMs);
    settings.endGroup();

    result.normalize();
    return result;
}

bool AppSettings::save(QSettings& settings) const
{
    settings.beginGroup(QStringLiteral("general"));
    settings.setValue(QStringLiteral("language"), language);
    settings.setValue(QStringLiteral("lightingEnabled"), lightingEnabled);
    settings.setValue(QStringLiteral("startWithWindows"), startWithWindows);
    settings.setValue(QStringLiteral("closeToTray"), closeToTray);
    settings.setValue(QStringLiteral("hotkey"), hotkey);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("screen"));
    settings.setValue(QStringLiteral("width"), screenWidth);
    settings.setValue(QStringLiteral("height"), screenHeight);
    settings.setValue(QStringLiteral("outputId"), captureOutputId);
    settings.setValue(QStringLiteral("targetFps"), targetFps);
    settings.setValue(QStringLiteral("captureMethod"), captureMethod);
    settings.setValue(QStringLiteral("mirrorOutputId"), mirrorOutputId);
    settings.setValue(QStringLiteral("mirrorFps"), mirrorFps);
    settings.setValue(QStringLiteral("gamingMode"), gamingMode);
    settings.setValue(QStringLiteral("samplingZones"), samplingZones);
    settings.setValue(
        QStringLiteral("automaticSamplingDepth"),
        automaticSamplingDepth);
    settings.setValue(
        QStringLiteral("samplingDepthPixels"),
        samplingDepthPixels);
    settings.setValue(QStringLiteral("regionColorMode"), regionColorMode);
    settings.setValue(QStringLiteral("ledTransferMode"), ledTransferMode);
    settings.setValue(
        QStringLiteral("blackBarDetectionMode"),
        blackBarDetectionMode);
    settings.remove(QStringLiteral("blackBarAutodetect"));
    settings.setValue(
        QStringLiteral("smoothingMilliseconds"),
        smoothingMilliseconds);
    settings.setValue(
        QStringLiteral("flashProtectionEnabled"),
        flashProtectionEnabled);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("ledStrip"));
    settings.setValue(QStringLiteral("type"), ledType);
    settings.setValue(QStringLiteral("colorOrder"), colorOrder);
    settings.setValue(QStringLiteral("top"), topLeds);
    settings.setValue(QStringLiteral("right"), rightLeds);
    settings.setValue(QStringLiteral("bottom"), bottomLeds);
    settings.setValue(QStringLiteral("left"), leftLeds);
    settings.setValue(QStringLiteral("offset"), ledOffset);
    settings.setValue(QStringLiteral("clockwise"), clockwise);
    settings.setValue(QStringLiteral("brightnessPercent"), brightnessPercent);
    settings.setValue(QStringLiteral("currentLimitMilliAmps"), currentLimitMilliAmps);
    settings.setValue(QStringLiteral("redCalibrationPercent"), redCalibrationPercent);
    settings.setValue(QStringLiteral("greenCalibrationPercent"), greenCalibrationPercent);
    settings.setValue(QStringLiteral("blueCalibrationPercent"), blueCalibrationPercent);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("effects"));
    settings.setValue(QStringLiteral("activeMode"), activeMode);
    settings.setValue(QStringLiteral("standaloneEffect"), standaloneEffect);
    settings.setValue(QStringLiteral("solidColor"), solidColor);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("audio"));
    settings.setValue(QStringLiteral("source"), audioSource);
    settings.setValue(QStringLiteral("outputDeviceId"), audioOutputDeviceId);
    settings.setValue(QStringLiteral("inputDeviceId"), audioInputDeviceId);
    settings.setValue(QStringLiteral("bandCount"), audioBandCount);
    settings.setValue(QStringLiteral("minimumFrequencyHz"), audioMinimumFrequencyHz);
    settings.setValue(QStringLiteral("maximumFrequencyHz"), audioMaximumFrequencyHz);
    settings.setValue(QStringLiteral("sensitivityPercent"), audioSensitivityPercent);
    settings.setValue(QStringLiteral("noiseGateDecibels"), audioNoiseGateDecibels);
    settings.setValue(QStringLiteral("outputGainPercent"), audioOutputGainPercent);
    settings.setValue(
        QStringLiteral("microphoneGainPercent"), audioMicrophoneGainPercent);
    settings.setValue(QStringLiteral("attackMilliseconds"), audioAttackMilliseconds);
    settings.setValue(QStringLiteral("releaseMilliseconds"), audioReleaseMilliseconds);
    settings.setValue(QStringLiteral("dynamics"), audioDynamics);
    settings.setValue(
        QStringLiteral("springinessPercent"), audioSpringinessPercent);
    settings.setValue(QStringLiteral("layout"), audioLayout);
    settings.setValue(QStringLiteral("colorMode"), audioColorMode);
    settings.setValue(QStringLiteral("solidColor"), audioSolidColor);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("connection"));
    settings.setValue(
        QStringLiteral("automaticPortSelection"),
        automaticPortSelection);
    settings.setValue(QStringLiteral("portName"), portName);
    settings.setValue(QStringLiteral("baudRate"), baudRate);
    settings.setValue(QStringLiteral("ambientTimeoutMs"), ambientTimeoutMs);
    settings.endGroup();

    settings.sync();
    return settings.status() == QSettings::NoError;
}

} // namespace ambilight
