#include "capture/capture_output_selection.hpp"
#include "capture/capture_types.hpp"

#include <QString>

#include <iostream>
#include <limits>

namespace {

bool expect(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

bool expectValid(
    const ambilight::capture::CaptureConfiguration& configuration,
    const char* message)
{
    QString reason;
    return expect(configuration.isValid(&reason) && reason.isEmpty(), message);
}

bool expectInvalid(
    const ambilight::capture::CaptureConfiguration& configuration,
    const char* message)
{
    QString reason;
    return expect(!configuration.isValid(&reason) && !reason.isEmpty(), message);
}

bool testValidConfigurationAndLayout()
{
    const ambilight::capture::CaptureConfiguration configuration;
    const ambilight::capture::SamplingLayout expected{
        .top = 96,
        .right = 54,
        .bottom = 96,
        .left = 54,
    };

    bool success = expectValid(configuration, "default configuration is valid");
    success &= expect(configuration.ledCount() == 300, "configuration LED total");
    success &= expect(
        configuration.samplingLayout() == expected,
        "sampling layout follows configured sides");
    success &= expect(expected.ledCount() == 300, "sampling layout LED total");
    return success;
}

bool testLedBoundaries()
{
    ambilight::capture::CaptureConfiguration configuration;
    configuration.topLedCount = 1;
    configuration.rightLedCount = 0;
    configuration.bottomLedCount = 0;
    configuration.leftLedCount = 0;

    bool success = expectValid(configuration, "one perimeter LED is valid");

    configuration.topLedCount = ambilight::capture::kMaximumLedCount;
    success &= expectValid(configuration, "maximum perimeter LED count is valid");

    configuration.topLedCount = 0;
    success &= expectInvalid(configuration, "empty perimeter is invalid");

    configuration.topLedCount = -1;
    success &= expectInvalid(configuration, "negative side LED count is invalid");

    configuration.topLedCount = ambilight::capture::kMaximumLedCount + 1;
    success &= expectInvalid(
        configuration,
        "side LED count above maximum is invalid");

    configuration.topLedCount = ambilight::capture::kMaximumLedCount;
    configuration.rightLedCount = 1;
    success &= expectInvalid(
        configuration,
        "perimeter LED total above maximum is invalid");
    return success;
}

bool testFpsBoundaries()
{
    ambilight::capture::CaptureConfiguration configuration;

    configuration.targetFps = 1;
    bool success = expectValid(configuration, "minimum target FPS is valid");

    configuration.targetFps = 240;
    success &= expectValid(configuration, "maximum target FPS is valid");

    configuration.targetFps = 0;
    success &= expectInvalid(configuration, "target FPS below minimum is invalid");

    configuration.targetFps = 241;
    success &= expectInvalid(configuration, "target FPS above maximum is invalid");
    return success;
}

bool testNonFiniteFloatFields()
{
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float infinity = std::numeric_limits<float>::infinity();

    ambilight::capture::CaptureConfiguration configuration;
    configuration.blackLuminanceThreshold = nan;
    bool success = expectInvalid(
        configuration,
        "NaN black luminance threshold is invalid");

    configuration = {};
    configuration.blackLuminanceThreshold = infinity;
    success &= expectInvalid(
        configuration,
        "infinite black luminance threshold is invalid");

    configuration = {};
    configuration.maximumBlackBarFraction = nan;
    success &= expectInvalid(
        configuration,
        "NaN maximum black-bar fraction is invalid");

    configuration = {};
    configuration.maximumBlackBarFraction = -infinity;
    success &= expectInvalid(
        configuration,
        "infinite maximum black-bar fraction is invalid");
    return success;
}

bool testBlackBarSearchLimits()
{
    using ambilight::capture::BlackBarDetectionMode;
    using ambilight::capture::blackBarSearchLimits;

    const auto disabled = blackBarSearchLimits(
        BlackBarDetectionMode::Off,
        QSize(3840, 2160));
    const auto firstContent = blackBarSearchLimits(
        BlackBarDetectionMode::FirstContent,
        QSize(3840, 2160));
    const auto limited16x9 = blackBarSearchLimits(
        BlackBarDetectionMode::Limited21x9,
        QSize(3840, 2160));
    const auto limitedUltrawide = blackBarSearchLimits(
        BlackBarDetectionMode::Limited21x9,
        QSize(3440, 1440));

    bool success = expect(
        disabled.horizontalFraction == 0.0F
            && disabled.verticalFraction == 0.0F
            && disabled.horizontalProbeFraction == 0.0F
            && disabled.verticalProbeFraction == 0.0F,
        "disabled black-bar search has no axes");
    success &= expect(
        std::abs(firstContent.horizontalFraction - 0.30F) < 0.0001F
            && std::abs(firstContent.verticalFraction - 0.30F) < 0.0001F
            && std::abs(firstContent.horizontalProbeFraction - 0.30F)
                < 0.0001F
            && std::abs(firstContent.verticalProbeFraction - 0.30F)
                < 0.0001F,
        "first-content search uses all sides");
    success &= expect(
        std::abs(limited16x9.horizontalFraction - 0.1190476F) < 0.0001F
            && limited16x9.verticalFraction == 0.0F
            && limited16x9.horizontalProbeFraction
                > limited16x9.horizontalFraction
            && limited16x9.verticalProbeFraction == 0.0F,
        "21:9 search caps a 16:9 frame and disables side search");
    success &= expect(
        limitedUltrawide.horizontalFraction == 0.0F
            && limitedUltrawide.verticalFraction == 0.0F,
        "21:9 search does not inset an already wider frame");

    ambilight::capture::CaptureConfiguration configuration;
    configuration.blackBarDetectionMode =
        static_cast<BlackBarDetectionMode>(255);
    success &= expectInvalid(
        configuration,
        "unknown black-bar mode is invalid");
    return success;
}

bool testSamplingDepth()
{
    using ambilight::capture::CaptureConfiguration;
    using ambilight::capture::CaptureInsets;
    using ambilight::capture::RegionColorMode;
    using ambilight::capture::LedTransferMode;
    using ambilight::capture::adaptiveDepthSampleCount;
    using ambilight::capture::automaticSamplingDepthPixels;
    using ambilight::capture::resolvedSamplingDepthPixels;
    using ambilight::capture::samplesPerZone;

    bool success = expect(
        automaticSamplingDepthPixels(QSize(3840, 2160)) == 120,
        "4K automatic sampling depth");
    success &= expect(
        automaticSamplingDepthPixels(QSize(3440, 1440)) == 80,
        "21:9 automatic depth follows the shorter dimension");
    success &= expect(
        automaticSamplingDepthPixels(QSize(2560, 1440)) == 80,
        "16:9 clone mode preserves depth at the same height");
    success &= expect(
        automaticSamplingDepthPixels(
            QSize(3840, 2160),
            CaptureInsets{
                .left = 0,
                .top = 280,
                .right = 0,
                .bottom = 280})
            == 88,
        "automatic depth follows the detected content area");
    success &= expect(
        automaticSamplingDepthPixels(QSize(100, 100)) == 8,
        "automatic sampling depth minimum");
    success &= expect(
        automaticSamplingDepthPixels(QSize(16'384, 16'384)) == 160,
        "automatic sampling depth maximum");
    success &= expect(
        resolvedSamplingDepthPixels(
            QSize(1280, 720),
            {},
            512)
            == 360,
        "manual sampling depth stops at the content center");
    success &= expect(
        adaptiveDepthSampleCount(8) == 8
            && adaptiveDepthSampleCount(120) == 8
            && adaptiveDepthSampleCount(128) == 8
            && adaptiveDepthSampleCount(129) == 9
            && adaptiveDepthSampleCount(160) == 10
            && adaptiveDepthSampleCount(512) == 32,
        "depth sample count adapts from 8 to 32 rows");
    success &= expect(
        samplesPerZone(120) == 32
            && samplesPerZone(512) == 128,
        "adaptive samples per zone");

    CaptureConfiguration configuration;
    configuration.samplingDepthPixels = 0;
    success &= expectValid(configuration, "automatic sampling depth is valid");
    configuration.samplingDepthPixels = 8;
    success &= expectValid(configuration, "minimum manual depth is valid");
    configuration.samplingDepthPixels = 512;
    success &= expectValid(configuration, "maximum manual depth is valid");
    configuration.samplingDepthPixels = -1;
    success &= expectInvalid(configuration, "negative sampling depth is invalid");
    configuration.samplingDepthPixels = 513;
    success &= expectInvalid(
        configuration,
        "manual sampling depth above maximum is invalid");
    configuration = {};
    configuration.regionColorMode = RegionColorMode::RootMeanSquare;
    success &= expectValid(configuration, "RMS region color mode is valid");
    configuration.regionColorMode = RegionColorMode::Dominant;
    success &= expectValid(configuration, "dominant region color mode is valid");
    configuration.regionColorMode = static_cast<RegionColorMode>(255);
    success &= expectInvalid(
        configuration,
        "unknown region color mode is invalid");
    configuration = {};
    configuration.ledTransferMode = LedTransferMode::Raw;
    success &= expectValid(configuration, "raw LED transfer mode is valid");
    configuration.ledTransferMode = LedTransferMode::SoftGamma;
    success &= expectValid(configuration, "soft LED transfer mode is valid");
    configuration.ledTransferMode = LedTransferMode::StrongGamma;
    success &= expectValid(configuration, "strong LED transfer mode is valid");
    configuration.ledTransferMode = static_cast<LedTransferMode>(255);
    success &= expectInvalid(
        configuration,
        "unknown LED transfer mode is invalid");
    return success;
}

bool testStableDisplayIdentity()
{
    const QString firstPath =
        QStringLiteral(
            R"(\\?\DISPLAY#SAM1234#5&12345&0&UID4357#{01234567-89ab-cdef-0123-456789abcdef})");
    const QString samePathDifferentCase = firstPath.toUpper();
    const QString otherPath =
        QStringLiteral(
            R"(\\?\DISPLAY#SAM1234#5&12345&0&UID4358#{01234567-89ab-cdef-0123-456789abcdef})");

    const QString first =
        ambilight::capture::stableCaptureOutputId(firstPath);
    const QString equivalent =
        ambilight::capture::stableCaptureOutputId(
            samePathDifferentCase);
    const QString other =
        ambilight::capture::stableCaptureOutputId(otherPath);

    return expect(
               first.startsWith(
                   QString::fromLatin1(
                       ambilight::capture::
                           kStableDisplayIdentityPrefix)),
               "stable display identity prefix")
        && expect(first == equivalent,
            "monitor device path identity is case-insensitive")
        && expect(first != other,
            "different monitor paths have different identities")
        && expect(!first.contains(QStringLiteral("SAM1234")),
            "stable identity does not persist the raw monitor path")
        && expect(
            ambilight::capture::stableCaptureOutputId({}).isEmpty(),
            "empty monitor path has no stable identity");
}

ambilight::capture::CaptureOutput makeOutput(
    const QString& id,
    const QString& legacyId,
    const bool primary)
{
    ambilight::capture::CaptureOutput output;
    output.id = id;
    output.legacyId = legacyId;
    output.label = legacyId;
    output.primary = primary;
    return output;
}

bool testDisplaySelectionAndLegacyMigration()
{
    const auto first = makeOutput(
        QStringLiteral("displayconfig-v1|first"),
        QStringLiteral(R"(\\.\DISPLAY1)"),
        true);
    const auto second = makeOutput(
        QStringLiteral("displayconfig-v1|second"),
        QStringLiteral(R"(\\.\DISPLAY2)"),
        false);
    const QVector<ambilight::capture::CaptureOutput> outputs{
        first,
        second};

    const auto* automatic =
        ambilight::capture::resolveCaptureOutput(outputs, {});
    const auto* stable = ambilight::capture::resolveCaptureOutput(
        outputs,
        second.id);
    const auto* legacy = ambilight::capture::resolveCaptureOutput(
        outputs,
        second.legacyId);
    const auto* unknown = ambilight::capture::resolveCaptureOutput(
        outputs,
        QStringLiteral("displayconfig-v1|missing"));

    auto withoutPrimary = outputs;
    withoutPrimary[0].primary = false;

    return expect(automatic != nullptr && automatic->id == first.id,
               "automatic selection resolves the primary output")
        && expect(stable != nullptr && stable->id == second.id,
            "stable display identity resolves its output")
        && expect(legacy != nullptr && legacy->id == second.id,
            "legacy DISPLAYn identity migrates to the stable output")
        && expect(unknown == nullptr,
            "unknown explicit display identity does not fall back")
        && expect(
            ambilight::capture::resolveCaptureOutput(
                withoutPrimary,
                {})
                    ->id
                == first.id,
            "automatic selection falls back to the first output");
}

bool testAutomaticPrimaryTransition()
{
    const auto first = makeOutput(
        QStringLiteral("displayconfig-v1|first"),
        QStringLiteral(R"(\\.\DISPLAY1)"),
        true);
    auto second = makeOutput(
        QStringLiteral("displayconfig-v1|second"),
        QStringLiteral(R"(\\.\DISPLAY2)"),
        false);
    QVector<ambilight::capture::CaptureOutput> outputs{first, second};

    const bool unchanged =
        !ambilight::capture::automaticCaptureOutputChanged(
            outputs,
            first.id,
            first.legacyId);

    // A temporary DisplayConfig lookup failure falls back to the GDI name.
    auto legacyFallback = first;
    legacyFallback.id = legacyFallback.legacyId;
    const bool fallbackStillMatches =
        !ambilight::capture::automaticCaptureOutputChanged(
            {legacyFallback, second},
            first.id,
            first.legacyId);

    outputs[0].primary = false;
    outputs[1].primary = true;
    const bool primaryChangeDetected =
        ambilight::capture::automaticCaptureOutputChanged(
            outputs,
            first.id,
            first.legacyId);
    const bool emptyEnumerationIsInconclusive =
        !ambilight::capture::automaticCaptureOutputChanged(
            {},
            first.id,
            first.legacyId);

    return expect(unchanged,
               "automatic selection keeps the current primary")
        && expect(fallbackStillMatches,
            "legacy fallback does not cause a false output switch")
        && expect(primaryChangeDetected,
            "automatic selection detects a new primary")
        && expect(emptyEnumerationIsInconclusive,
            "transient empty enumeration does not tear down capture");
}

bool testGamingPerimeterCopies()
{
    using ambilight::capture::perimeterCopyRegions;
    // Exhaustive small surfaces cover portrait/landscape, odd dimensions,
    // overlapping borders, and depths larger than the entire frame.
    for (int width = 1; width <= 17; ++width) {
        for (int height = 1; height <= 17; ++height) {
            for (int depth = 1; depth <= 20; ++depth) {
                const auto regions = perimeterCopyRegions({width, height}, depth);
                const QRect bounds(0, 0, width, height);
                for (const auto& region : regions) {
                    if (!expect(!region.isEmpty() && bounds.contains(region),
                            "copy boxes must stay inside the source/destination")) {
                        return false;
                    }
                }
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        int copies = 0;
                        for (const auto& region : regions) {
                            copies += region.contains(x, y) ? 1 : 0;
                        }
                        const bool edge = x < depth || y < depth
                            || x >= width - depth || y >= height - depth;
                        if (!expect(copies == (edge ? 1 : 0),
                                "every sampled border pixel is copied once; center is skipped")) {
                            return false;
                        }
                    }
                }
            }
        }
    }
    qint64 pixels = 0;
    for (const auto& region : perimeterCopyRegions({3840, 2160}, 120)) {
        pixels += static_cast<qint64>(region.width()) * region.height();
    }
    return expect(pixels == 1'382'400, "4K auto-depth copies one sixth of the full frame")
        && expect(perimeterCopyRegions({}, 120).isEmpty(), "invalid surface is not copied");
}

} // namespace

int main()
{
    const bool success = testValidConfigurationAndLayout()
        && testGamingPerimeterCopies()
        && testLedBoundaries()
        && testFpsBoundaries()
        && testNonFiniteFloatFields()
        && testBlackBarSearchLimits()
        && testSamplingDepth()
        && testStableDisplayIdentity()
        && testDisplaySelectionAndLegacyMigration()
        && testAutomaticPrimaryTransition();
    if (success) {
        std::cout << "All capture type tests passed.\n";
    }
    return success ? 0 : 1;
}
