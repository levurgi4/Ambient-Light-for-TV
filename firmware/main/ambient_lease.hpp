#pragma once

#include <cstdint>

namespace ambilight::firmware {

[[nodiscard]] constexpr bool is_ambient_lease_active(
    const bool has_ambient_frame,
    const std::uint64_t now_milliseconds,
    const std::uint64_t received_at_milliseconds,
    const std::uint16_t timeout_milliseconds) noexcept {
    return has_ambient_frame &&
           now_milliseconds >= received_at_milliseconds &&
           now_milliseconds - received_at_milliseconds <=
               timeout_milliseconds;
}

}  // namespace ambilight::firmware
