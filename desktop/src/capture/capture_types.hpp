#pragma once

#include <QMetaType>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <QtGlobal>
#include <QtGui/qrgb.h>

#include <algorithm>
#include <cmath>

namespace ambilight::capture {

inline constexpr int kMaximumLedCount = 300;
inline constexpr int kSamplesAlongZone = 4;
inline constexpr int kMinimumDepthSamples = 8;
inline constexpr int kMaximumDepthSamples = 32;
inline constexpr int kTargetDepthSampleSpacingPixels = 16;

enum class RegionColorMode : quint8 {
    Mean = 0,
    RootMeanSquare = 1,
    Dominant = 2,
};

enum class LedTransferMode : quint8 {
    Raw = 0,
    SoftGamma = 1,
    Srgb = 2,
    StrongGamma = 3,
};

enum class BlackBarDetectionMode : quint8 {
    Off = 0,
    FirstContent = 1,
    Limited21x9 = 2,
};

struct BlackBarSearchLimits final {
    float horizontalFraction = 0.0F;
    float verticalFraction = 0.0F;
    float horizontalProbeFraction = 0.0F;
    float verticalProbeFraction = 0.0F;

    friend bool operator==(
        const BlackBarSearchLimits&,
        const BlackBarSearchLimits&) = default;
};

[[nodiscard]] inline BlackBarSearchLimits blackBarSearchLimits(
    const BlackBarDetectionMode mode,
    const QSize& frameSize,
    const float maximumFraction = 0.30F) noexcept
{
    switch (mode) {
    case BlackBarDetectionMode::Off:
        return {};
    case BlackBarDetectionMode::FirstContent:
        return {
            maximumFraction,
            maximumFraction,
            maximumFraction,
            maximumFraction};
    case BlackBarDetectionMode::Limited21x9:
        if (frameSize.width() <= 0 || frameSize.height() <= 0) {
            return {};
        }
        // The target content height is width / (21 / 9). Half of the
        // remaining height is the maximum letterbox inset on each edge.
        // Wider frames already contain no 21:9 letterbox area to search.
        constexpr double targetAspectRatio = 21.0 / 9.0;
        const double frameAspectRatio =
            static_cast<double>(frameSize.width())
            / static_cast<double>(frameSize.height());
        const double horizontalFraction =
            0.5 * (1.0 - frameAspectRatio / targetAspectRatio);
        const float insetFraction = static_cast<float>(std::clamp(
                horizontalFraction,
                0.0,
                static_cast<double>(maximumFraction)));
        // Probe a few scan lines beyond the cap so an exact 21:9 boundary
        // still provides two consecutive visible-content samples. The
        // returned inset remains clamped to insetFraction.
        const float probeFraction = std::min(
            maximumFraction,
            insetFraction * 1.04F);
        return {insetFraction, 0.0F, probeFraction, 0.0F};
    }
    return {};
}

struct CaptureOutput final {
    // Stable application identity derived from the active DisplayConfig target
    // when Windows exposes one. Falls back to legacyId when a stable target
    // identity is temporarily unavailable.
    QString id;
    // Current GDI source name (for example "\\.\DISPLAY1"). This is useful
    // for diagnostics and for migrating settings written by older versions,
    // but it is not stable across display-topology changes.
    QString legacyId;
    QString label;
    QRect geometry;
    bool primary = false;

    friend bool operator==(const CaptureOutput&, const CaptureOutput&) = default;
};

struct CaptureInsets final {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    [[nodiscard]] QRect contentRect(const QSize& frameSize) const noexcept
    {
        const int width = std::max(0, frameSize.width() - left - right);
        const int height = std::max(0, frameSize.height() - top - bottom);
        return {left, top, width, height};
    }

    friend bool operator==(const CaptureInsets&, const CaptureInsets&) = default;
};

[[nodiscard]] inline int automaticSamplingDepthPixels(
    const QSize& frameSize,
    const CaptureInsets& contentInsets = {}) noexcept
{
    const QSize contentSize = contentInsets.contentRect(frameSize).size();
    const int shorterDimension =
        std::min(contentSize.width(), contentSize.height());
    return std::clamp(shorterDimension / 18, 8, 160);
}

[[nodiscard]] inline int resolvedSamplingDepthPixels(
    const QSize& frameSize,
    const CaptureInsets& contentInsets,
    const int configuredDepthPixels) noexcept
{
    const QSize contentSize = contentInsets.contentRect(frameSize).size();
    const int maximumUsableDepth =
        std::max(1, std::min(contentSize.width(), contentSize.height()) / 2);
    const int requestedDepth = configuredDepthPixels > 0
        ? configuredDepthPixels
        : automaticSamplingDepthPixels(frameSize, contentInsets);
    return std::min(requestedDepth, maximumUsableDepth);
}

[[nodiscard]] inline int adaptiveDepthSampleCount(
    const int samplingDepthPixels) noexcept
{
    const int positiveDepth = std::max(1, samplingDepthPixels);
    const int samplesForTargetSpacing =
        (positiveDepth + kTargetDepthSampleSpacingPixels - 1)
        / kTargetDepthSampleSpacingPixels;
    return std::clamp(
        samplesForTargetSpacing,
        kMinimumDepthSamples,
        kMaximumDepthSamples);
}

[[nodiscard]] inline int samplesPerZone(
    const int samplingDepthPixels) noexcept
{
    return kSamplesAlongZone
        * adaptiveDepthSampleCount(samplingDepthPixels);
}

struct SamplingLayout final {
    int top = 0;
    int right = 0;
    int bottom = 0;
    int left = 0;

    [[nodiscard]] int ledCount() const noexcept
    {
        return top + right + bottom + left;
    }

    friend bool operator==(const SamplingLayout&, const SamplingLayout&) = default;
};

struct CaptureConfiguration final {
    QString outputId;

    int topLedCount = 96;
    int rightLedCount = 54;
    int bottomLedCount = 96;
    int leftLedCount = 54;

    int targetFps = 60;
    BlackBarDetectionMode blackBarDetectionMode =
        BlackBarDetectionMode::FirstContent;

    // A pixel is considered black below this SDR luminance. Desktop
    // Duplication always exposes BGRA8 SDR data for the supported path.
    float blackLuminanceThreshold = 0.030F;
    float maximumBlackBarFraction = 0.30F;

    // Zero selects an adaptive depth (roughly 1/18 of the shorter content
    // dimension). A fixed value is useful for calibration and diagnostics.
    int samplingDepthPixels = 0;
    RegionColorMode regionColorMode = RegionColorMode::Mean;
    LedTransferMode ledTransferMode = LedTransferMode::Srgb;

    [[nodiscard]] int ledCount() const noexcept
    {
        return topLedCount + rightLedCount + bottomLedCount + leftLedCount;
    }

    [[nodiscard]] bool isValid(QString* reason = nullptr) const
    {
        const auto reject = [reason](const QString& text) {
            if (reason != nullptr) {
                *reason = text;
            }
            return false;
        };

        const int counts[] = {
            topLedCount, rightLedCount, bottomLedCount, leftLedCount};
        for (const int count : counts) {
            if (count < 0 || count > kMaximumLedCount) {
                return reject(QStringLiteral(
                    "Each side must contain from 0 to %1 LEDs.")
                                  .arg(kMaximumLedCount));
            }
        }

        if (ledCount() < 1 || ledCount() > kMaximumLedCount) {
            return reject(QStringLiteral(
                "The perimeter must contain from 1 to %1 LEDs.")
                              .arg(kMaximumLedCount));
        }
        if (targetFps < 1 || targetFps > 240) {
            return reject(QStringLiteral("Target FPS must be between 1 and 240."));
        }
        if (!std::isfinite(blackLuminanceThreshold)
            || blackLuminanceThreshold < 0.001F
            || blackLuminanceThreshold > 0.20F) {
            return reject(QStringLiteral(
                "Black luminance threshold must be between 0.001 and 0.20."));
        }
        if (!std::isfinite(maximumBlackBarFraction)
            || maximumBlackBarFraction < 0.05F
            || maximumBlackBarFraction > 0.45F) {
            return reject(QStringLiteral(
                "Maximum black-bar fraction must be between 0.05 and 0.45."));
        }
        if (samplingDepthPixels < 0 || samplingDepthPixels > 512) {
            return reject(QStringLiteral(
                "Sampling depth must be between 0 and 512 pixels."));
        }
        switch (regionColorMode) {
        case RegionColorMode::Mean:
        case RegionColorMode::RootMeanSquare:
        case RegionColorMode::Dominant:
            break;
        default:
            return reject(QStringLiteral(
                "The region color-processing mode is invalid."));
        }
        switch (blackBarDetectionMode) {
        case BlackBarDetectionMode::Off:
        case BlackBarDetectionMode::FirstContent:
        case BlackBarDetectionMode::Limited21x9:
            break;
        default:
            return reject(QStringLiteral(
                "The black-bar detection mode is invalid."));
        }
        switch (ledTransferMode) {
        case LedTransferMode::Raw:
        case LedTransferMode::SoftGamma:
        case LedTransferMode::Srgb:
        case LedTransferMode::StrongGamma:
            break;
        default:
            return reject(QStringLiteral(
                "The LED transfer mode is invalid."));
        }
        return true;
    }

    [[nodiscard]] SamplingLayout samplingLayout() const noexcept
    {
        return {
            topLedCount, rightLedCount, bottomLedCount, leftLedCount};
    }

    friend bool operator==(
        const CaptureConfiguration&,
        const CaptureConfiguration&) = default;
};

struct CaptureSample final {
    // Canonical clockwise perimeter:
    // top L->R, right T->B, bottom R->L, left B->T.
    QVector<QRgb> colors;
    SamplingLayout samplingLayout;
    quint64 configurationGeneration = 0;
    QSize frameSize;
    CaptureInsets contentInsets;
    double framesPerSecond = 0.0;
    double processingMilliseconds = 0.0;
    quint64 frameNumber = 0;
    qint64 capturedAtMilliseconds = 0;
};

enum class CaptureState : quint8 {
    Stopped,
    Starting,
    Running,
    WaitingForFrame,
    Reinitializing,
    Error
};

struct CaptureStatus final {
    CaptureState state = CaptureState::Stopped;
    QString outputId;
    QString message;
    qint64 nativeError = 0;

    friend bool operator==(const CaptureStatus&, const CaptureStatus&) = default;
};

} // namespace ambilight::capture

Q_DECLARE_METATYPE(ambilight::capture::CaptureOutput)
Q_DECLARE_METATYPE(QVector<ambilight::capture::CaptureOutput>)
Q_DECLARE_METATYPE(ambilight::capture::CaptureInsets)
Q_DECLARE_METATYPE(ambilight::capture::RegionColorMode)
Q_DECLARE_METATYPE(ambilight::capture::LedTransferMode)
Q_DECLARE_METATYPE(ambilight::capture::BlackBarDetectionMode)
Q_DECLARE_METATYPE(ambilight::capture::SamplingLayout)
Q_DECLARE_METATYPE(ambilight::capture::CaptureConfiguration)
Q_DECLARE_METATYPE(ambilight::capture::CaptureSample)
Q_DECLARE_METATYPE(ambilight::capture::CaptureState)
Q_DECLARE_METATYPE(ambilight::capture::CaptureStatus)
