#pragma once

#include <cstdint>
#include <span>

#include "firmware_types.hpp"

namespace ambilight::firmware {

inline constexpr std::size_t kLedBytesPerPixel = 3;

// Converts logical RGB pixels to the byte order expected on the strip wire.
// The destination must contain exactly three bytes per source pixel.
[[nodiscard]] bool encode_led_wire_bytes(
    std::span<const Rgb> pixels,
    ColorOrder order,
    std::span<std::uint8_t> destination) noexcept;

}  // namespace ambilight::firmware
