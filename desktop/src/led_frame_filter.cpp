#include "led_frame_filter.hpp"

#include <algorithm>
#include <cmath>

namespace ambilight {
namespace {

constexpr int kMaximumFrameDeltaMilliseconds = 50;
constexpr int kInitialFrameDeltaMilliseconds = 16;
constexpr double kFullBrightnessRiseMilliseconds = 180.0;

[[nodiscard]] double luminance(const QColor& color) noexcept
{
    return 0.2126 * static_cast<double>(color.red())
        + 0.7152 * static_cast<double>(color.green())
        + 0.0722 * static_cast<double>(color.blue());
}

[[nodiscard]] double meanLuminance(const QVector<QColor>& colors) noexcept
{
    if (colors.isEmpty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (const auto& color : colors) {
        sum += luminance(color);
    }
    return sum / static_cast<double>(colors.size());
}

[[nodiscard]] int filteredChannel(
    const int previous,
    const int target,
    const double alpha) noexcept
{
    return std::clamp(
        static_cast<int>(std::lround(
            static_cast<double>(previous)
            + (static_cast<double>(target - previous) * alpha))),
        0,
        255);
}

} // namespace

QVector<QColor> LedFrameFilter::apply(
    const QVector<QColor>& target,
    const qint64 timestampMilliseconds,
    const int smoothingMilliseconds,
    const bool flashProtectionEnabled)
{
    if (target.isEmpty()) {
        reset();
        return {};
    }
    if (previous_.size() != target.size() || !hasPreviousTimestamp_) {
        if (!flashProtectionEnabled) {
            previous_ = target;
            previousTimestampMilliseconds_ = timestampMilliseconds;
            hasPreviousTimestamp_ = true;
            return target;
        }
        previous_.fill(QColor(Qt::black), target.size());
        previousTimestampMilliseconds_ =
            timestampMilliseconds - kInitialFrameDeltaMilliseconds;
        hasPreviousTimestamp_ = true;
    }

    const int elapsedMilliseconds = static_cast<int>(std::clamp<qint64>(
        timestampMilliseconds - previousTimestampMilliseconds_,
        1,
        kMaximumFrameDeltaMilliseconds));
    previousTimestampMilliseconds_ = timestampMilliseconds;

    const int timeConstant = std::clamp(smoothingMilliseconds, 0, 500);
    const double alpha = timeConstant == 0
        ? 1.0
        : 1.0 - std::exp(
            -static_cast<double>(elapsedMilliseconds)
            / static_cast<double>(timeConstant));
    QVector<QColor> filtered;
    filtered.reserve(target.size());
    for (qsizetype index = 0; index < target.size(); ++index) {
        filtered.push_back(QColor::fromRgb(
            filteredChannel(
                previous_[index].red(), target[index].red(), alpha),
            filteredChannel(
                previous_[index].green(), target[index].green(), alpha),
            filteredChannel(
                previous_[index].blue(), target[index].blue(), alpha)));
    }

    if (flashProtectionEnabled) {
        const double previousMean = meanLuminance(previous_);
        const double filteredMean = meanLuminance(filtered);
        const double allowedRise = 255.0
            * static_cast<double>(elapsedMilliseconds)
            / kFullBrightnessRiseMilliseconds;
        const double limitedMean = previousMean + allowedRise;
        if (filteredMean > limitedMean && filteredMean > 0.0) {
            const double scale =
                std::clamp(limitedMean / filteredMean, 0.0, 1.0);
            for (auto& color : filtered) {
                color.setRgb(
                    static_cast<int>(std::lround(color.red() * scale)),
                    static_cast<int>(std::lround(color.green() * scale)),
                    static_cast<int>(std::lround(color.blue() * scale)));
            }
        }
    }
    previous_ = filtered;
    return filtered;
}

void LedFrameFilter::reset() noexcept
{
    previous_.clear();
    previousTimestampMilliseconds_ = 0;
    hasPreviousTimestamp_ = false;
}

} // namespace ambilight
