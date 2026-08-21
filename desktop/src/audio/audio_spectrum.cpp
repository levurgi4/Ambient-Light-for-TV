#include "audio_spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace ambilight::audio {

namespace {

constexpr int kFftSize = 1024;
constexpr int kFftHopSize = kFftSize / 2;

void fft(std::vector<std::complex<float>>& values)
{
    const std::size_t size = values.size();
    for (std::size_t index = 1, reversed = 0; index < size; ++index) {
        std::size_t bit = size >> 1U;
        while ((reversed & bit) != 0U) {
            reversed ^= bit;
            bit >>= 1U;
        }
        reversed ^= bit;
        if (index < reversed) {
            std::swap(values[index], values[reversed]);
        }
    }

    for (std::size_t length = 2; length <= size; length <<= 1U) {
        const float angle = -2.0F * std::numbers::pi_v<float>
            / static_cast<float>(length);
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (std::size_t offset = 0; offset < size; offset += length) {
            std::complex<float> factor(1.0F, 0.0F);
            for (std::size_t index = 0; index < length / 2U; ++index) {
                const auto even = values[offset + index];
                const auto odd = values[offset + index + length / 2U] * factor;
                values[offset + index] = even + odd;
                values[offset + index + length / 2U] = even - odd;
                factor *= step;
            }
        }
    }
}

float smoothingAlpha(const double elapsedMilliseconds, const int timeMilliseconds)
{
    if (timeMilliseconds <= 0 || elapsedMilliseconds <= 0.0) {
        return 1.0F;
    }
    return static_cast<float>(1.0 - std::exp(
        -elapsedMilliseconds / static_cast<double>(timeMilliseconds)));
}

QColor scaledColor(const QColor& color, const float level)
{
    const float clamped = std::clamp(level, 0.0F, 1.0F);
    return {
        static_cast<int>(std::lround(color.red() * clamped)),
        static_cast<int>(std::lround(color.green() * clamped)),
        static_cast<int>(std::lround(color.blue() * clamped)),
    };
}

} // namespace

bool SpectrumAnalyzerConfiguration::isValid() const noexcept
{
    return bandCount >= 4 && bandCount <= 64
        && minimumFrequencyHz >= 20
        && maximumFrequencyHz > minimumFrequencyHz
        && maximumFrequencyHz <= 24'000
        && sensitivityPercent >= 10 && sensitivityPercent <= 400
        && noiseGateDecibels >= -100 && noiseGateDecibels <= -10;
}

int SpectrumPerimeter::total() const noexcept
{
    return top + right + bottom + left;
}

bool SpectrumPerimeter::isValid() const noexcept
{
    return top >= 0 && right >= 0 && bottom >= 0 && left >= 0
        && total() > 0;
}

int SpectrumPerimeter::bottomCenterIndex() const noexcept
{
    return top + right + (bottom / 2);
}

AudioSpectrumAnalyzer::AudioSpectrumAnalyzer(
    SpectrumAnalyzerConfiguration configuration)
    : configuration_(configuration.isValid()
              ? configuration
              : SpectrumAnalyzerConfiguration{})
{
    rebuildWindow();
    reset();
}

void AudioSpectrumAnalyzer::setConfiguration(
    const SpectrumAnalyzerConfiguration& configuration)
{
    configuration_ = configuration.isValid()
        ? configuration
        : SpectrumAnalyzerConfiguration{};
    reset();
}

void AudioSpectrumAnalyzer::reset()
{
    pendingSamples_.clear();
    pendingSamples_.reserve(kFftSize * 2);
    levels_.assign(
        static_cast<std::size_t>(configuration_.bandCount),
        0.0F);
    fftBuffer_.assign(kFftSize, {});
}

bool AudioSpectrumAnalyzer::pushSamples(
    const std::span<const float> monoSamples,
    const int sampleRate)
{
    if (sampleRate < 8'000 || sampleRate > 384'000 || monoSamples.empty()) {
        return false;
    }

    pendingSamples_.insert(
        pendingSamples_.end(), monoSamples.begin(), monoSamples.end());
    bool analyzed = false;
    while (pendingSamples_.size() >= kFftSize) {
        analyzeWindow(sampleRate);
        pendingSamples_.erase(
            pendingSamples_.begin(),
            pendingSamples_.begin() + kFftHopSize);
        analyzed = true;
    }
    return analyzed;
}

const std::vector<float>& AudioSpectrumAnalyzer::levels() const noexcept
{
    return levels_;
}

void AudioSpectrumAnalyzer::rebuildWindow()
{
    window_.resize(kFftSize);
    for (int index = 0; index < kFftSize; ++index) {
        window_[static_cast<std::size_t>(index)] = 0.5F
            - 0.5F * std::cos(
                2.0F * std::numbers::pi_v<float>
                * static_cast<float>(index)
                / static_cast<float>(kFftSize - 1));
    }
}

void AudioSpectrumAnalyzer::analyzeWindow(const int sampleRate)
{
    float windowSum = 0.0F;
    for (int index = 0; index < kFftSize; ++index) {
        const float weight = window_[static_cast<std::size_t>(index)];
        fftBuffer_[static_cast<std::size_t>(index)] =
            std::complex<float>(
                pendingSamples_[static_cast<std::size_t>(index)] * weight,
                0.0F);
        windowSum += weight;
    }
    fft(fftBuffer_);

    const float nyquist = static_cast<float>(sampleRate) / 2.0F;
    const float minimumFrequency = std::min(
        static_cast<float>(configuration_.minimumFrequencyHz),
        nyquist * 0.9F);
    const float maximumFrequency = std::max(
        minimumFrequency + 1.0F,
        std::min(
            static_cast<float>(configuration_.maximumFrequencyHz),
            nyquist));
    const float amplitudeScale = 2.0F / std::max(1.0F, windowSum);
    const float sensitivityGain =
        static_cast<float>(configuration_.sensitivityPercent) / 100.0F;
    const float gate = static_cast<float>(configuration_.noiseGateDecibels);

    for (int band = 0; band < configuration_.bandCount; ++band) {
        const float startRatio =
            static_cast<float>(band) / configuration_.bandCount;
        const float endRatio =
            static_cast<float>(band + 1) / configuration_.bandCount;
        const float startFrequency = minimumFrequency
            * std::pow(maximumFrequency / minimumFrequency, startRatio);
        const float endFrequency = minimumFrequency
            * std::pow(maximumFrequency / minimumFrequency, endRatio);
        const int startBin = std::clamp(
            static_cast<int>(std::floor(
                startFrequency * kFftSize / sampleRate)),
            1,
            (kFftSize / 2) - 1);
        const int endBin = std::clamp(
            static_cast<int>(std::ceil(
                endFrequency * kFftSize / sampleRate)),
            startBin + 1,
            kFftSize / 2);

        float magnitude = 0.0F;
        for (int bin = startBin; bin < endBin; ++bin) {
            magnitude = std::max(
                magnitude,
                std::abs(fftBuffer_[static_cast<std::size_t>(bin)])
                    * amplitudeScale);
        }
        magnitude *= sensitivityGain;
        const float decibels = 20.0F * std::log10(
            std::max(magnitude, 1.0e-8F));
        float normalized = (decibels - gate) / -gate;
        normalized = std::clamp(normalized, 0.0F, 1.0F);
        levels_[static_cast<std::size_t>(band)] =
            std::pow(normalized, 1.5F);
    }
}

void SpectrumSmoother::reset()
{
    levels_.clear();
    envelopeLevels_.clear();
    velocities_.clear();
}

std::vector<float> SpectrumSmoother::apply(
    const std::span<const float> targetLevels,
    const double elapsedMilliseconds,
    const int attackMilliseconds,
    const int releaseMilliseconds,
    const SpectrumDynamics dynamics,
    const int springinessPercent)
{
    if (levels_.size() != targetLevels.size()) {
        levels_.assign(targetLevels.size(), 0.0F);
        envelopeLevels_.assign(targetLevels.size(), 0.0F);
        velocities_.assign(targetLevels.size(), 0.0F);
    }
    for (std::size_t index = 0; index < targetLevels.size(); ++index) {
        const float target = std::clamp(targetLevels[index], 0.0F, 1.0F);
        const int time = target >= envelopeLevels_[index]
            ? attackMilliseconds
            : releaseMilliseconds;
        const float alpha = smoothingAlpha(elapsedMilliseconds, time);
        envelopeLevels_[index] +=
            (target - envelopeLevels_[index]) * alpha;

        if (dynamics == SpectrumDynamics::Smooth) {
            levels_[index] = envelopeLevels_[index];
            velocities_[index] = 0.0F;
        } else {
            // A damped second-order system gives every frequency band its own
            // position and velocity. Small fixed substeps keep it stable even
            // when the audio thread briefly misses a frame.
            const float strength = std::clamp(
                static_cast<float>(springinessPercent) / 100.0F,
                0.0F,
                1.0F);
            const float dampingRatio = 0.92F - (0.70F * strength);
            constexpr float naturalFrequencyRadians =
                2.0F * std::numbers::pi_v<float> * 5.5F;
            double remainingSeconds = std::clamp(
                elapsedMilliseconds, 0.0, 50.0) / 1'000.0;
            while (remainingSeconds > 0.0) {
                const float step = static_cast<float>(
                    std::min(remainingSeconds, 0.008));
                const float acceleration =
                    naturalFrequencyRadians * naturalFrequencyRadians
                        * (envelopeLevels_[index] - levels_[index])
                    - 2.0F * dampingRatio * naturalFrequencyRadians
                        * velocities_[index];
                velocities_[index] += acceleration * step;
                levels_[index] += velocities_[index] * step;
                remainingSeconds -= step;
            }
            if (levels_[index] < 0.0F) {
                levels_[index] = 0.0F;
                velocities_[index] = std::max(
                    0.0F, -velocities_[index] * 0.12F * strength);
            } else if (levels_[index] > 1.15F) {
                levels_[index] = 1.15F;
                velocities_[index] = std::min(
                    0.0F, -velocities_[index] * 0.08F * strength);
            }
        }
        if (levels_[index] < 0.0001F) {
            levels_[index] = 0.0F;
        }
    }
    return levels_;
}

QVector<QColor> makeSpectrumLedColors(
    const std::span<const float> bandLevels,
    const SpectrumPerimeter& perimeter,
    const SpectrumLayout layout,
    const SpectrumColorMode colorMode,
    const QColor& solidColor)
{
    if (bandLevels.empty() || !perimeter.isValid()) {
        return {};
    }

    const int ledCount = perimeter.total();
    const int bandCount = static_cast<int>(bandLevels.size());
    const int anchor = std::clamp(
        perimeter.bottomCenterIndex(), 0, ledCount - 1);
    QVector<QColor> colors;
    colors.reserve(ledCount);

    for (int led = 0; led < ledCount; ++led) {
        int band = 0;
        if (layout == SpectrumLayout::Mirrored) {
            const int clockwiseDistance = (led - anchor + ledCount) % ledCount;
            const int counterclockwiseDistance =
                (anchor - led + ledCount) % ledCount;
            const int distance =
                std::min(clockwiseDistance, counterclockwiseDistance);
            const int maximumDistance = std::max(1, ledCount / 2);
            band = std::min(
                bandCount - 1,
                distance * bandCount / (maximumDistance + 1));
        } else {
            const int distance = (led - anchor + ledCount) % ledCount;
            band = std::min(
                bandCount - 1,
                distance * bandCount / std::max(1, ledCount));
        }

        QColor base = solidColor;
        if (colorMode == SpectrumColorMode::FrequencyGradient) {
            const double ratio = bandCount <= 1
                ? 0.0
                : static_cast<double>(band) / (bandCount - 1);
            base = QColor::fromHsvF(0.72 * ratio, 1.0, 1.0);
        }
        colors.push_back(scaledColor(
            base, bandLevels[static_cast<std::size_t>(band)]));
    }
    return colors;
}

} // namespace ambilight::audio
