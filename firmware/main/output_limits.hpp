#pragma once

#include <cstdint>
#include <span>

#include "firmware_types.hpp"

namespace ambilight::firmware {

// Conservative software estimate used to keep the configured PSU budget from
// being exceeded by RGB content. The fixed idle part cannot be reduced in
// software; the dynamic part can.
inline constexpr std::uint32_t kEstimatedMilliampsPerFullChannel = 12;

struct OutputCurrentEstimate {
    std::uint64_t idle_milliamps{};
    std::uint64_t dynamic_milliamps{};
    std::uint64_t total_milliamps{};
};

[[nodiscard]] OutputCurrentEstimate estimate_output_current(
    std::span<const Rgb> pixels) noexcept;

void apply_output_limits(
    std::span<Rgb> pixels,
    Rgb color_calibration,
    std::uint8_t brightness_limit,
    std::uint16_t current_limit_milliamps) noexcept;

}  // namespace ambilight::firmware
