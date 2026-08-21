#pragma once

#include <QColor>
#include <QVector>

#include <complex>
#include <span>
#include <vector>

namespace ambilight::audio {

enum class SpectrumLayout {
    Mirrored,
    Sequential,
};

enum class SpectrumColorMode {
    FrequencyGradient,
    Solid,
};

enum class SpectrumDynamics {
    Smooth,
    Spring,
};

struct SpectrumAnalyzerConfiguration final {
    int bandCount{24};
    int minimumFrequencyHz{40};
    int maximumFrequencyHz{16'000};
    int sensitivityPercent{100};
    int noiseGateDecibels{-55};

    [[nodiscard]] bool isValid() const noexcept;
};

struct SpectrumPerimeter final {
    int top{};
    int right{};
    int bottom{};
    int left{};

    [[nodiscard]] int total() const noexcept;
    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] int bottomCenterIndex() const noexcept;
};

class AudioSpectrumAnalyzer final {
public:
    explicit AudioSpectrumAnalyzer(
        SpectrumAnalyzerConfiguration configuration = {});

    void setConfiguration(
        const SpectrumAnalyzerConfiguration& configuration);
    void reset();
    [[nodiscard]] bool pushSamples(
        std::span<const float> monoSamples,
        int sampleRate);
    [[nodiscard]] const std::vector<float>& levels() const noexcept;

private:
    void rebuildWindow();
    void analyzeWindow(int sampleRate);

    SpectrumAnalyzerConfiguration configuration_;
    std::vector<float> pendingSamples_;
    std::vector<float> window_;
    std::vector<float> levels_;
    std::vector<std::complex<float>> fftBuffer_;
};

class SpectrumSmoother final {
public:
    void reset();
    [[nodiscard]] std::vector<float> apply(
        std::span<const float> targetLevels,
        double elapsedMilliseconds,
        int attackMilliseconds,
        int releaseMilliseconds,
        SpectrumDynamics dynamics = SpectrumDynamics::Smooth,
        int springinessPercent = 55);

private:
    std::vector<float> levels_;
    std::vector<float> envelopeLevels_;
    std::vector<float> velocities_;
};

[[nodiscard]] QVector<QColor> makeSpectrumLedColors(
    std::span<const float> bandLevels,
    const SpectrumPerimeter& perimeter,
    SpectrumLayout layout,
    SpectrumColorMode colorMode,
    const QColor& solidColor);

} // namespace ambilight::audio
