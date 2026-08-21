#pragma once

#include <cstddef>
#include <cstdint>

namespace ambilight::firmware {

// Convert a physical strip position to the logical clockwise perimeter index
// used by the desktop mapper. Ambient frames are already sent in physical
// order; autonomous spatial effects use this helper to match their orientation.
[[nodiscard]] constexpr std::size_t logical_index_for_physical(
    const std::size_t physical_index,
    const std::size_t led_count,
    const std::uint16_t start_offset,
    const bool reversed) noexcept {
    if (led_count == 0) {
        return 0;
    }

    const auto offset =
        static_cast<std::size_t>(start_offset) % led_count;
    const auto physical = physical_index % led_count;
    return reversed
        ? (offset + led_count - physical) % led_count
        : (offset + physical) % led_count;
}

}  // namespace ambilight::firmware
