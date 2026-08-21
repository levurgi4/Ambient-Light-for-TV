#pragma once

#include <cstdint>

namespace ambilight::firmware {

// Produces integer tick intervals whose long-term sum exactly matches the
// requested fractional rate. For 1000 ticks/s at 60 FPS this yields a stable
// 16/17/17 ms cadence instead of the drifting 16 ms approximation.
class FractionalTickPacer final {
public:
    constexpr FractionalTickPacer(
        const std::uint32_t ticks_per_second,
        const std::uint32_t frames_per_second) noexcept
        : whole_ticks_(ticks_per_second / frames_per_second),
          remainder_per_frame_(ticks_per_second % frames_per_second),
          frames_per_second_(frames_per_second) {}

    [[nodiscard]] constexpr std::uint32_t next_interval() noexcept {
        auto interval = whole_ticks_;
        accumulated_remainder_ += remainder_per_frame_;
        if (accumulated_remainder_ >= frames_per_second_) {
            accumulated_remainder_ -= frames_per_second_;
            ++interval;
        }
        return interval;
    }

    constexpr void reset_phase() noexcept {
        accumulated_remainder_ = 0;
    }

private:
    std::uint32_t whole_ticks_{};
    std::uint32_t remainder_per_frame_{};
    std::uint32_t frames_per_second_{};
    std::uint32_t accumulated_remainder_{};
};

}  // namespace ambilight::firmware
