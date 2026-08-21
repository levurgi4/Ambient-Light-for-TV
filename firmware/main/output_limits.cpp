#include "output_limits.hpp"

#include <algorithm>
#include <cstdint>

namespace ambilight::firmware {
namespace {

[[nodiscard]] constexpr std::uint8_t scale_channel_rounded(
    const std::uint8_t channel,
    const std::uint8_t factor) noexcept {
    return static_cast<std::uint8_t>(
        (static_cast<std::uint16_t>(channel) * factor + 127U) / 255U);
}

[[nodiscard]] constexpr std::uint8_t scale_channel_down(
    const std::uint8_t channel,
    const std::uint8_t factor) noexcept {
    // Rounding down is intentional for the safety limiter. Together with the
    // conservative current estimate it guarantees that the reducible part
    // does not cross its integer milliamp budget after quantization.
    return static_cast<std::uint8_t>(
        static_cast<std::uint16_t>(channel) * factor / 255U);
}

}  // namespace

OutputCurrentEstimate estimate_output_current(
    const std::span<const Rgb> pixels) noexcept {
    std::uint64_t summed_channels = 0;
    for (const auto pixel : pixels) {
        summed_channels += static_cast<std::uint64_t>(pixel.red) +
                           pixel.green +
                           pixel.blue;
    }

    const auto idle_milliamps =
        static_cast<std::uint64_t>(pixels.size()) *
        kEstimatedIdleMilliampsPerPixel;
    const auto dynamic_milliamps =
        (summed_channels * kEstimatedMilliampsPerFullChannel + 254U) /
        255U;
    return {
        .idle_milliamps = idle_milliamps,
        .dynamic_milliamps = dynamic_milliamps,
        .total_milliamps = idle_milliamps + dynamic_milliamps,
    };
}

void apply_output_limits(
    const std::span<Rgb> pixels,
    const Rgb color_calibration,
    const std::uint8_t brightness_limit,
    const std::uint16_t current_limit_milliamps) noexcept {
    for (auto& pixel : pixels) {
        pixel = {
            scale_channel_rounded(
                scale_channel_rounded(pixel.red, color_calibration.red),
                brightness_limit),
            scale_channel_rounded(
                scale_channel_rounded(pixel.green, color_calibration.green),
                brightness_limit),
            scale_channel_rounded(
                scale_channel_rounded(pixel.blue, color_calibration.blue),
                brightness_limit),
        };
    }

    const auto estimate = estimate_output_current(pixels);
    if (estimate.total_milliamps <= current_limit_milliamps ||
        estimate.dynamic_milliamps == 0) {
        return;
    }

    const auto dynamic_budget =
        current_limit_milliamps > estimate.idle_milliamps
        ? static_cast<std::uint64_t>(current_limit_milliamps) -
              estimate.idle_milliamps
        : 0U;
    const auto current_scale = static_cast<std::uint8_t>(
        std::min<std::uint64_t>(
            255U,
            dynamic_budget * 255U / estimate.dynamic_milliamps));
    for (auto& pixel : pixels) {
        pixel = {
            scale_channel_down(pixel.red, current_scale),
            scale_channel_down(pixel.green, current_scale),
            scale_channel_down(pixel.blue, current_scale),
        };
    }
}

}  // namespace ambilight::firmware
