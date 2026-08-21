#include "led_frame_filter.hpp"

#include <QColor>
#include <QVector>

#include <iostream>

namespace {

bool expect(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

bool testUnfilteredFramesPassThrough()
{
    ambilight::LedFrameFilter filter;
    const QVector<QColor> black{Qt::black, Qt::black};
    const QVector<QColor> target{Qt::red, Qt::blue};
    static_cast<void>(filter.apply(black, 1'000, 0, false));
    return expect(
        filter.apply(target, 1'016, 0, false) == target,
        "disabled filter passes the target frame through");
}

bool testTemporalSmoothing()
{
    ambilight::LedFrameFilter filter;
    static_cast<void>(filter.apply({Qt::black}, 1'000, 60, false));
    const auto filtered = filter.apply({Qt::white}, 1'016, 60, false);
    const int channel = filtered.front().red();
    return expect(
               channel >= 58 && channel <= 61,
               "60 ms smoothing produces the expected first transition step")
        && expect(
            filtered.front().red() == filtered.front().green()
                && filtered.front().green() == filtered.front().blue(),
            "temporal smoothing preserves neutral white");
}

bool testFlashProtectionLimitsOnlyRises()
{
    ambilight::LedFrameFilter filter;
    const auto firstRise = filter.apply({Qt::white}, 1'000, 0, true);
    const auto secondRise = filter.apply({Qt::white}, 1'016, 0, true);
    const auto decrease = filter.apply({Qt::black}, 1'032, 0, true);
    return expect(
               firstRise.front().red() >= 22
                   && firstRise.front().red() <= 24,
               "flash protection caps a sudden full-frame rise")
        && expect(
            secondRise.front().red() > firstRise.front().red()
                && secondRise.front().red() <= 47,
            "a persistent bright scene ramps up instead of remaining blocked")
        && expect(
            decrease.front() == QColor(Qt::black),
            "flash protection does not delay a brightness decrease");
}

bool testLayoutChangeStartsFresh()
{
    ambilight::LedFrameFilter filter;
    static_cast<void>(filter.apply({Qt::black}, 1'000, 120, false));
    const QVector<QColor> resized{Qt::white, Qt::red};
    return expect(
        filter.apply(resized, 1'016, 120, false) == resized,
        "a changed zone count resets temporal history");
}

bool testSmoothingAndFlashGuardDoNotStackRiseTimes()
{
    ambilight::LedFrameFilter filter;
    const auto filtered = filter.apply({Qt::white}, 1'000, 25, true);
    return expect(
        filtered.front().red() >= 22 && filtered.front().red() <= 24,
        "flash protection caps the smoothed result without double filtering");
}

} // namespace

int main()
{
    const bool success = testUnfilteredFramesPassThrough()
        && testTemporalSmoothing()
        && testFlashProtectionLimitsOnlyRises()
        && testLayoutChangeStartsFresh()
        && testSmoothingAndFlashGuardDoNotStackRiseTimes();
    if (success) {
        std::cout << "All LED frame filter tests passed.\n";
    }
    return success ? 0 : 1;
}
