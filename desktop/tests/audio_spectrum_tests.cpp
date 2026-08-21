#include "audio/audio_spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <vector>

namespace {

bool expect(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

std::vector<float> sineWave(const float frequency, const int sampleRate)
{
    std::vector<float> samples(2048);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        samples[index] = 0.8F * std::sin(
            2.0F * std::numbers::pi_v<float> * frequency
            * static_cast<float>(index) / sampleRate);
    }
    return samples;
}

int strongestBand(const std::vector<float>& levels)
{
    return static_cast<int>(std::distance(
        levels.begin(),
        std::max_element(levels.begin(), levels.end())));
}

bool testFrequencySeparation()
{
    constexpr int sampleRate = 48'000;
    ambilight::audio::AudioSpectrumAnalyzer lowAnalyzer;
    ambilight::audio::AudioSpectrumAnalyzer highAnalyzer;
    const auto low = sineWave(100.0F, sampleRate);
    const auto high = sineWave(4'000.0F, sampleRate);
    const bool lowReady = lowAnalyzer.pushSamples(low, sampleRate);
    const bool highReady = highAnalyzer.pushSamples(high, sampleRate);
    const int lowBand = strongestBand(lowAnalyzer.levels());
    const int highBand = strongestBand(highAnalyzer.levels());

    return expect(lowReady && highReady, "FFT windows analyzed")
        && expect(lowBand < highBand, "low and high tones are separated")
        && expect(highBand - lowBand >= 6, "logarithmic bands have useful spread");
}

bool testSilence()
{
    ambilight::audio::AudioSpectrumAnalyzer analyzer;
    const std::vector<float> silence(2048, 0.0F);
    static_cast<void>(analyzer.pushSamples(silence, 48'000));
    return expect(
        std::all_of(
            analyzer.levels().begin(),
            analyzer.levels().end(),
            [](const float value) { return value == 0.0F; }),
        "silence stays dark");
}

bool testSmoothing()
{
    ambilight::audio::SpectrumSmoother smoother;
    const std::vector<float> loud{1.0F};
    const std::vector<float> quiet{0.0F};
    const auto attacked = smoother.apply(loud, 20.0, 20, 200);
    const auto released = smoother.apply(quiet, 20.0, 20, 200);
    return expect(attacked[0] > 0.5F, "attack responds quickly")
        && expect(released[0] > 0.4F, "release decays gradually")
        && expect(released[0] < attacked[0], "release moves toward silence");
}

bool testSpringDynamics()
{
    ambilight::audio::SpectrumSmoother smoother;
    const std::vector<float> target{0.65F};
    float maximum = 0.0F;
    float minimumAfterOvershoot = 1.0F;
    bool overshot = false;
    float last = 0.0F;
    for (int frame = 0; frame < 120; ++frame) {
        const auto levels = smoother.apply(
            target,
            16.0,
            0,
            180,
            ambilight::audio::SpectrumDynamics::Spring,
            65);
        last = levels[0];
        maximum = std::max(maximum, last);
        if (last > target[0] + 0.01F) {
            overshot = true;
        }
        if (overshot) {
            minimumAfterOvershoot = std::min(minimumAfterOvershoot, last);
        }
    }
    return expect(maximum > target[0] + 0.03F, "spring overshoots the target")
        && expect(minimumAfterOvershoot < target[0] - 0.005F,
            "spring rebounds across the target")
        && expect(std::abs(last - target[0]) < 0.002F,
            "spring settles at the requested level");
}

bool testSpringStaysBoundedAfterFrameHitches()
{
    ambilight::audio::SpectrumSmoother smoother;
    const std::vector<float> loud{1.0F};
    const std::vector<float> quiet{0.0F};
    for (int frame = 0; frame < 100; ++frame) {
        const auto levels = smoother.apply(
            (frame % 2) == 0 ? loud : quiet,
            250.0,
            0,
            10,
            ambilight::audio::SpectrumDynamics::Spring,
            100);
        if (!std::isfinite(levels[0])
            || levels[0] < 0.0F || levels[0] > 1.15F) {
            return expect(false, "spring remains bounded after frame hitches");
        }
    }
    return true;
}

bool testMirroredPerimeter()
{
    const std::vector<float> levels{1.0F, 0.0F};
    const ambilight::audio::SpectrumPerimeter perimeter{4, 2, 4, 2};
    const auto colors = ambilight::audio::makeSpectrumLedColors(
        levels,
        perimeter,
        ambilight::audio::SpectrumLayout::Mirrored,
        ambilight::audio::SpectrumColorMode::Solid,
        Qt::white);
    const int bottomCenter = perimeter.bottomCenterIndex();
    const int topCenter = perimeter.top / 2;
    return expect(colors.size() == perimeter.total(), "perimeter frame size")
        && expect(colors[bottomCenter].red() == 255, "bass starts at bottom center")
        && expect(colors[topCenter].red() == 0, "treble reaches opposite side");
}

} // namespace

int main()
{
    const bool success = testFrequencySeparation()
        && testSilence()
        && testSmoothing()
        && testSpringDynamics()
        && testSpringStaysBoundedAfterFrameHitches()
        && testMirroredPerimeter();
    if (success) {
        std::cout << "All audio spectrum tests passed.\n";
    }
    return success ? 0 : 1;
}
