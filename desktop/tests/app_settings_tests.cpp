#include "app_settings.hpp"

#include <QSettings>
#include <QTemporaryDir>

#include <iostream>

namespace {

bool expect(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

bool testDefaults()
{
    const ambilight::AppSettings settings;
    return expect(settings.ledType == QStringLiteral("WS2815"), "default LED type")
        && expect(settings.ledCount() == 300, "default LED count")
        && expect(settings.screenWidth == 3840, "default screen width")
        && expect(settings.screenHeight == 2160, "default screen height")
        && expect(settings.captureOutputId.isEmpty(), "default capture output")
        && expect(settings.captureMethod == QStringLiteral("dxgi"),
            "existing installations retain DXGI capture")
        && expect(settings.targetFps == 60, "default target frame rate")
        && expect(!settings.gamingMode, "existing installations keep normal capture")
        && expect(settings.automaticSamplingDepth, "default automatic sampling depth")
        && expect(settings.samplingDepthPixels == 120, "default manual sampling depth")
        && expect(settings.regionColorMode == QStringLiteral("mean"),
            "default region color mode")
        && expect(settings.ledTransferMode == QStringLiteral("srgb"),
            "default LED transfer mode")
        && expect(
            settings.blackBarDetectionMode == QStringLiteral("first-content"),
            "default black-bar mode")
        && expect(settings.smoothingMilliseconds == 25,
            "default temporal smoothing")
        && expect(settings.flashProtectionEnabled,
            "default flash protection")
        && expect(settings.audioSource == QStringLiteral("windows"),
            "default audio source")
        && expect(settings.audioBandCount == 24, "default audio bands")
        && expect(settings.audioLayout == QStringLiteral("mirrored"),
            "default audio layout")
        && expect(settings.audioDynamics == QStringLiteral("spring"),
            "default audio dynamics")
        && expect(settings.audioSpringinessPercent == 55,
            "default audio springiness")
        && expect(settings.solidColor == QStringLiteral("#ff6018"),
            "default solid color")
        && expect(settings.automaticPortSelection, "default automatic COM selection")
        && expect(settings.baudRate == 2'000'000, "default baud rate")
        && expect(settings.currentLimitMilliAmps == 5000, "default current limit")
        && expect(settings.redCalibrationPercent == 100, "default red calibration")
        && expect(settings.greenCalibrationPercent == 100, "default green calibration")
        && expect(settings.blueCalibrationPercent == 100, "default blue calibration")
        && expect(settings.ambientTimeoutMs == 1000, "default stream timeout");
}

bool testWindowCaptureMethodPersistence()
{
    QTemporaryDir directory;
    if (!directory.isValid()) return false;
    QSettings storage(directory.filePath(QStringLiteral("window.ini")), QSettings::IniFormat);
    ambilight::AppSettings settings;
    settings.captureMethod = QStringLiteral("wgc_window");
    if (!settings.save(storage)) return false;
    const auto restored = ambilight::AppSettings::load(storage);
    return expect(restored.captureMethod == QStringLiteral("wgc_window"), "WGC window method survives restart")
        && expect(!storage.contains(QStringLiteral("sourceWindow")), "HWND is not persisted")
        && expect(!storage.contains(QStringLiteral("sourceWindowProcessId")), "process identity is not persisted");
}

bool testNormalization()
{
    ambilight::AppSettings settings;
    settings.language = QStringLiteral("unknown");
    settings.captureMethod = QStringLiteral("unknown");
    settings.ledType = QStringLiteral("unsupported");
    settings.colorOrder = QStringLiteral("XYZ");
    settings.activeMode = QStringLiteral("unknown");
    settings.standaloneEffect = QStringLiteral("unknown");
    settings.solidColor = QStringLiteral("not-a-color");
    settings.audioSource = QStringLiteral("unknown");
    settings.audioLayout = QStringLiteral("unknown");
    settings.audioColorMode = QStringLiteral("unknown");
    settings.audioDynamics = QStringLiteral("unknown");
    settings.audioSolidColor = QStringLiteral("not-a-color");
    settings.audioBandCount = 999;
    settings.audioMinimumFrequencyHz = 10'000;
    settings.audioMaximumFrequencyHz = 500;
    settings.audioSensitivityPercent = 999;
    settings.audioNoiseGateDecibels = 0;
    settings.audioOutputGainPercent = 999;
    settings.audioMicrophoneGainPercent = -1;
    settings.audioAttackMilliseconds = 999;
    settings.audioReleaseMilliseconds = 0;
    settings.audioSpringinessPercent = 999;
    settings.captureOutputId = QStringLiteral("  dxgi:0:1  ");
    settings.screenWidth = -1;
    settings.screenHeight = 100'000;
    settings.targetFps = 0;
    settings.samplingZones = 900;
    settings.samplingDepthPixels = 10'000;
    settings.smoothingMilliseconds = 77;
    settings.regionColorMode = QStringLiteral("unknown");
    settings.ledTransferMode = QStringLiteral("unknown");
    settings.blackBarDetectionMode = QStringLiteral("unknown");
    settings.topLeds = 0;
    settings.rightLeds = 0;
    settings.bottomLeds = 0;
    settings.leftLeds = 0;
    settings.ledOffset = 99'999;
    settings.brightnessPercent = 900;
    settings.currentLimitMilliAmps = 90'000;
    settings.redCalibrationPercent = -1;
    settings.greenCalibrationPercent = 101;
    settings.blueCalibrationPercent = 900;
    settings.baudRate = 115'200;
    settings.ambientTimeoutMs = 0;
    settings.automaticPortSelection = false;
    settings.portName = QStringLiteral("   ");
    settings.normalize();

    if (!expect(settings.captureMethod == QStringLiteral("dxgi"),
            "unknown capture methods fall back to DXGI")) {
        return false;
    }

    ambilight::AppSettings minimumDepthSettings;
    minimumDepthSettings.samplingDepthPixels = 0;
    minimumDepthSettings.normalize();

    return expect(settings.language == QStringLiteral("ru"), "language normalization")
        && expect(settings.ledType == QStringLiteral("WS2815"), "LED type normalization")
        && expect(settings.colorOrder == QStringLiteral("RGB"), "color order normalization")
        && expect(settings.activeMode == QStringLiteral("ambilight"), "mode normalization")
        && expect(settings.standaloneEffect == QStringLiteral("rainbow"),
            "effect normalization")
        && expect(settings.solidColor == QStringLiteral("#ff6018"),
            "solid color normalization")
        && expect(settings.audioSource == QStringLiteral("windows"),
            "audio source normalization")
        && expect(settings.audioLayout == QStringLiteral("mirrored"),
            "audio layout normalization")
        && expect(settings.audioColorMode == QStringLiteral("spectrum"),
            "audio color mode normalization")
        && expect(settings.audioDynamics == QStringLiteral("spring"),
            "audio dynamics normalization")
        && expect(settings.audioSolidColor == QStringLiteral("#00aaff"),
            "audio color normalization")
        && expect(settings.audioBandCount == 64, "audio band range")
        && expect(settings.audioMinimumFrequencyHz == 40
                && settings.audioMaximumFrequencyHz == 16'000,
            "audio frequency range normalization")
        && expect(settings.audioSensitivityPercent == 400,
            "audio sensitivity range")
        && expect(settings.audioNoiseGateDecibels == -10,
            "audio noise gate range")
        && expect(settings.audioOutputGainPercent == 200,
            "audio output gain range")
        && expect(settings.audioMicrophoneGainPercent == 0,
            "audio microphone gain range")
        && expect(settings.audioAttackMilliseconds == 500,
            "audio attack range")
        && expect(settings.audioReleaseMilliseconds == 10,
            "audio release range")
        && expect(settings.audioSpringinessPercent == 100,
            "audio springiness range")
        && expect(settings.captureOutputId == QStringLiteral("dxgi:0:1"),
            "capture output whitespace normalization")
        && expect(settings.screenWidth == 320, "minimum screen width")
        && expect(settings.screenHeight == 16'384, "maximum screen height")
        && expect(settings.targetFps == 1, "minimum target frame rate")
        && expect(settings.samplingZones == 300, "maximum sampling zones")
        && expect(settings.samplingDepthPixels == 512, "maximum sampling depth")
        && expect(settings.smoothingMilliseconds == 25,
            "unsupported smoothing normalization")
        && expect(settings.regionColorMode == QStringLiteral("mean"),
            "region color mode normalization")
        && expect(settings.ledTransferMode == QStringLiteral("srgb"),
            "LED transfer mode normalization")
        && expect(
            settings.blackBarDetectionMode == QStringLiteral("first-content"),
            "black-bar mode normalization")
        && expect(
            minimumDepthSettings.samplingDepthPixels == 8,
            "minimum sampling depth")
        && expect(settings.ledCount() == 300, "zero layout fallback")
        && expect(settings.ledOffset == 299, "LED offset range")
        && expect(settings.brightnessPercent == 100, "brightness range")
        && expect(settings.currentLimitMilliAmps == 10'000, "current limit range")
        && expect(settings.redCalibrationPercent == 0, "red calibration range")
        && expect(settings.greenCalibrationPercent == 100, "green calibration range")
        && expect(settings.blueCalibrationPercent == 100, "blue calibration range")
        && expect(settings.baudRate == 2'000'000, "firmware baud normalization")
        && expect(settings.ambientTimeoutMs == 100, "stream timeout range")
        && expect(settings.portName == QStringLiteral("COM3"), "empty COM fallback");
}

bool testRoundTrip()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "temporary settings directory")) {
        return false;
    }

    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    ambilight::AppSettings original;
    original.language = QStringLiteral("en");
    original.lightingEnabled = false;
    original.screenWidth = 3440;
    original.screenHeight = 1440;
    original.captureOutputId = QStringLiteral("dxgi:1:0");
    original.targetFps = 120;
    original.captureMethod = QStringLiteral("wgc");
    original.mirrorOutputId = QStringLiteral("mirror-test-display");
    original.mirrorFps = 30;
    original.gamingMode = true;
    original.samplingZones = 256;
    original.automaticSamplingDepth = false;
    original.samplingDepthPixels = 144;
    original.regionColorMode = QStringLiteral("dominant");
    original.ledTransferMode = QStringLiteral("strong-gamma");
    original.blackBarDetectionMode = QStringLiteral("limit-21-9");
    original.smoothingMilliseconds = 120;
    original.flashProtectionEnabled = false;
    original.topLeds = 90;
    original.rightLeds = 45;
    original.bottomLeds = 90;
    original.leftLeds = 45;
    original.ledOffset = 17;
    original.clockwise = false;
    original.redCalibrationPercent = 100;
    original.greenCalibrationPercent = 92;
    original.blueCalibrationPercent = 78;
    original.standaloneEffect = QStringLiteral("wave");
    original.solidColor = QStringLiteral("#123456");
    original.activeMode = QStringLiteral("equalizer");
    original.audioSource = QStringLiteral("mixed");
    original.audioOutputDeviceId = QStringLiteral("output-id");
    original.audioInputDeviceId = QStringLiteral("input-id");
    original.audioBandCount = 32;
    original.audioMinimumFrequencyHz = 55;
    original.audioMaximumFrequencyHz = 18'000;
    original.audioSensitivityPercent = 140;
    original.audioNoiseGateDecibels = -62;
    original.audioOutputGainPercent = 90;
    original.audioMicrophoneGainPercent = 125;
    original.audioAttackMilliseconds = 15;
    original.audioReleaseMilliseconds = 240;
    original.audioDynamics = QStringLiteral("spring");
    original.audioSpringinessPercent = 72;
    original.audioLayout = QStringLiteral("sequential");
    original.audioColorMode = QStringLiteral("solid");
    original.audioSolidColor = QStringLiteral("#112233");
    original.automaticPortSelection = false;
    original.portName = QStringLiteral("COM12");
    original.baudRate = 2'000'000;
    original.ambientTimeoutMs = 2500;

    {
        QSettings writer(path, QSettings::IniFormat);
        if (!expect(original.save(writer), "settings save")) {
            return false;
        }
    }

    QSettings reader(path, QSettings::IniFormat);
    const auto restored = ambilight::AppSettings::load(reader);
    return expect(restored.language == original.language, "language round trip")
        && expect(restored.lightingEnabled == original.lightingEnabled,
            "enabled state round trip")
        && expect(restored.screenWidth == original.screenWidth, "width round trip")
        && expect(restored.screenHeight == original.screenHeight, "height round trip")
        && expect(restored.captureOutputId == original.captureOutputId,
            "capture output round trip")
        && expect(restored.targetFps == original.targetFps, "FPS round trip")
        && expect(restored.captureMethod == original.captureMethod,
            "Windows Graphics Capture selection survives restart")
        && expect(restored.mirrorOutputId == original.mirrorOutputId
            && restored.mirrorFps == original.mirrorFps, "mirror preferences survive restart")
        && expect(restored.gamingMode, "game mode survives restart")
        && expect(restored.samplingZones == original.samplingZones, "zones round trip")
        && expect(
            restored.automaticSamplingDepth
                == original.automaticSamplingDepth,
            "automatic sampling depth round trip")
        && expect(
            restored.samplingDepthPixels
                == original.samplingDepthPixels,
            "sampling depth round trip")
        && expect(restored.regionColorMode == original.regionColorMode,
            "region color mode round trip")
        && expect(restored.ledTransferMode == original.ledTransferMode,
            "LED transfer mode round trip")
        && expect(
            restored.blackBarDetectionMode == original.blackBarDetectionMode,
            "black-bar mode round trip")
        && expect(
            restored.smoothingMilliseconds == original.smoothingMilliseconds,
            "temporal smoothing round trip")
        && expect(
            restored.flashProtectionEnabled
                == original.flashProtectionEnabled,
            "flash protection round trip")
        && expect(restored.ledCount() == original.ledCount(), "layout round trip")
        && expect(restored.ledOffset == original.ledOffset, "offset round trip")
        && expect(restored.clockwise == original.clockwise, "direction round trip")
        && expect(
            restored.redCalibrationPercent == original.redCalibrationPercent,
            "red calibration round trip")
        && expect(
            restored.greenCalibrationPercent == original.greenCalibrationPercent,
            "green calibration round trip")
        && expect(
            restored.blueCalibrationPercent == original.blueCalibrationPercent,
            "blue calibration round trip")
        && expect(restored.standaloneEffect == original.standaloneEffect,
            "standalone effect round trip")
        && expect(restored.solidColor == original.solidColor,
            "solid color round trip")
        && expect(restored.activeMode == original.activeMode,
            "active mode round trip")
        && expect(restored.audioSource == original.audioSource,
            "audio source round trip")
        && expect(restored.audioOutputDeviceId == original.audioOutputDeviceId,
            "audio output device round trip")
        && expect(restored.audioInputDeviceId == original.audioInputDeviceId,
            "audio input device round trip")
        && expect(restored.audioBandCount == original.audioBandCount,
            "audio bands round trip")
        && expect(
            restored.audioMinimumFrequencyHz == original.audioMinimumFrequencyHz
                && restored.audioMaximumFrequencyHz
                    == original.audioMaximumFrequencyHz,
            "audio frequency range round trip")
        && expect(restored.audioSensitivityPercent
                == original.audioSensitivityPercent,
            "audio sensitivity round trip")
        && expect(restored.audioNoiseGateDecibels
                == original.audioNoiseGateDecibels,
            "audio noise gate round trip")
        && expect(restored.audioOutputGainPercent
                == original.audioOutputGainPercent,
            "audio output gain round trip")
        && expect(restored.audioMicrophoneGainPercent
                == original.audioMicrophoneGainPercent,
            "audio microphone gain round trip")
        && expect(restored.audioAttackMilliseconds
                == original.audioAttackMilliseconds,
            "audio attack round trip")
        && expect(restored.audioReleaseMilliseconds
                == original.audioReleaseMilliseconds,
            "audio release round trip")
        && expect(restored.audioDynamics == original.audioDynamics,
            "audio dynamics round trip")
        && expect(restored.audioSpringinessPercent
                == original.audioSpringinessPercent,
            "audio springiness round trip")
        && expect(restored.audioLayout == original.audioLayout,
            "audio layout round trip")
        && expect(restored.audioColorMode == original.audioColorMode,
            "audio color mode round trip")
        && expect(restored.audioSolidColor == original.audioSolidColor,
            "audio color round trip")
        && expect(
            restored.automaticPortSelection == original.automaticPortSelection,
            "automatic COM selection round trip")
        && expect(restored.portName == original.portName, "port round trip")
        && expect(restored.baudRate == original.baudRate, "baud round trip")
        && expect(restored.ambientTimeoutMs == original.ambientTimeoutMs,
            "stream timeout round trip");
}

bool testOversizedLayoutNormalization()
{
    ambilight::AppSettings settings;
    settings.topLeds = 400;
    settings.rightLeds = 200;
    settings.bottomLeds = 400;
    settings.leftLeds = 200;
    settings.normalize();

    return expect(settings.ledCount() == 300, "maximum LED count")
        && expect(settings.topLeds == 100, "scaled top count")
        && expect(settings.rightLeds == 50, "scaled right count")
        && expect(settings.bottomLeds == 100, "scaled bottom count")
        && expect(settings.leftLeds == 50, "scaled left count");
}

bool testLegacyBlackBarSettingMigration()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "legacy settings directory")) {
        return false;
    }

    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    {
        QSettings writer(path, QSettings::IniFormat);
        writer.setValue(
            QStringLiteral("screen/blackBarAutodetect"),
            false);
    }

    QSettings reader(path, QSettings::IniFormat);
    const auto migrated = ambilight::AppSettings::load(reader);
    return expect(
        migrated.blackBarDetectionMode == QStringLiteral("off"),
        "legacy disabled black-bar checkbox migration");
}

bool testCurrentLimitAccountsForIdleDraw()
{
    ambilight::AppSettings settings;
    settings.topLeds = 100;
    settings.rightLeds = 50;
    settings.bottomLeds = 100;
    settings.leftLeds = 50;
    settings.currentLimitMilliAmps = 100;
    settings.normalize();
    if (!expect(
            settings.currentLimitMilliAmps == 900,
            "300 LED idle current floor")) {
        return false;
    }

    settings.topLeds = 10;
    settings.rightLeds = 10;
    settings.bottomLeds = 10;
    settings.leftLeds = 10;
    settings.currentLimitMilliAmps = 1;
    settings.normalize();
    return expect(
               ambilight::AppSettings::minimumCurrentLimitMilliAmps(40) == 120,
               "idle current floor calculation")
        && expect(
            settings.currentLimitMilliAmps == 120,
            "small layout idle current floor");
}

} // namespace

int main()
{
    const bool success = testDefaults()
        && testWindowCaptureMethodPersistence()
        && testNormalization()
        && testRoundTrip()
        && testOversizedLayoutNormalization()
        && testLegacyBlackBarSettingMigration()
        && testCurrentLimitAccountsForIdleDraw();
    if (success) {
        std::cout << "All desktop settings tests passed.\n";
    }
    return success ? 0 : 1;
}
