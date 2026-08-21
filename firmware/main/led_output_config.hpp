#pragma once

#include <cstdint>

namespace ambilight::firmware {

inline constexpr int kDisabledLedDataGpio = -1;

// Early WS2815/WS2815B strips and the current WS2815B-V1 silicon use
// incompatible low times for a data bit of one. Keep the selection at build
// time: a wrong profile cannot be made reliable by runtime retries.
enum class Ws2815TimingProfile : std::uint8_t {
    classic,
    b_v1_revised,
};

struct Ws2815Timing {
    std::uint32_t resolution_hz{};
    std::uint16_t zero_high_ticks{};
    std::uint16_t zero_low_ticks{};
    std::uint16_t one_high_ticks{};
    std::uint16_t one_low_ticks{};
    std::uint16_t reset_half_ticks{};

    [[nodiscard]] constexpr bool operator==(
        const Ws2815Timing&) const = default;
};

inline constexpr std::uint32_t kWs2815RmtResolutionHz = 8'000'000;

// Classic WS2815 timing: T0H=250 ns, T0L=1000 ns, T1H=1000 ns,
// T1L=250 ns. This remains the default because unqualified retail strips are
// most commonly sold simply as "WS2815" and this preserves compatibility with
// the original firmware.
inline constexpr Ws2815Timing kWs2815ClassicTiming{
    .resolution_hz = kWs2815RmtResolutionHz,
    .zero_high_ticks = 2,
    .zero_low_ticks = 8,
    .one_high_ticks = 8,
    .one_low_ticks = 2,
    .reset_half_ticks = 1200,
};

// WS2815B-V1 timing from the revised Worldsemi specification:
// T0H=250 ns, T0L=1000 ns, T1H=750 ns, T1L=625 ns.
// Both bit periods are at least 1.25 us and reset is 300 us.
inline constexpr Ws2815Timing kWs2815BV1Timing{
    .resolution_hz = kWs2815RmtResolutionHz,
    .zero_high_ticks = 2,
    .zero_low_ticks = 8,
    .one_high_ticks = 6,
    .one_low_ticks = 5,
    .reset_half_ticks = 1200,
};

[[nodiscard]] constexpr Ws2815Timing ws2815_timing(
    const Ws2815TimingProfile profile) noexcept {
    switch (profile) {
        case Ws2815TimingProfile::classic:
            return kWs2815ClassicTiming;
        case Ws2815TimingProfile::b_v1_revised:
            return kWs2815BV1Timing;
    }
    return {};
}

[[nodiscard]] constexpr bool is_valid(
    const Ws2815Timing timing) noexcept {
    constexpr std::uint16_t maximum_rmt_symbol_duration = 0x7FFF;
    if (timing.resolution_hz == 0 ||
        timing.zero_high_ticks == 0 ||
        timing.zero_low_ticks == 0 ||
        timing.one_high_ticks == 0 ||
        timing.one_low_ticks == 0 ||
        timing.reset_half_ticks == 0 ||
        timing.zero_high_ticks > maximum_rmt_symbol_duration ||
        timing.zero_low_ticks > maximum_rmt_symbol_duration ||
        timing.one_high_ticks > maximum_rmt_symbol_duration ||
        timing.one_low_ticks > maximum_rmt_symbol_duration ||
        timing.reset_half_ticks > maximum_rmt_symbol_duration) {
        return false;
    }

    const auto zero_ticks = static_cast<std::uint64_t>(
        timing.zero_high_ticks) + timing.zero_low_ticks;
    const auto one_ticks = static_cast<std::uint64_t>(
        timing.one_high_ticks) + timing.one_low_ticks;
    const auto reset_ticks =
        static_cast<std::uint64_t>(timing.reset_half_ticks) * 2ULL;

    return zero_ticks * 1'000'000'000ULL >=
               1'250ULL * timing.resolution_hz &&
           one_ticks * 1'000'000'000ULL >=
               1'250ULL * timing.resolution_hz &&
           reset_ticks * 1'000'000'000ULL >=
               280'000ULL * timing.resolution_hz;
}

// Conservative allow-list for an ESP32-WROOM-32D. It excludes UART0 (GPIO1/3),
// flash pins (GPIO6-11), input-only GPIOs and boot-strapping pins. A board can
// still reserve one of these pins, so selecting a pin remains an explicit
// installation-time decision.
[[nodiscard]] constexpr bool is_safe_led_data_gpio(const int gpio) noexcept {
    switch (gpio) {
        case 16:
        case 17:
        case 18:
        case 19:
        case 21:
        case 22:
        case 23:
        case 25:
        case 26:
        case 27:
        case 32:
        case 33:
            return true;
        default:
            return false;
    }
}

}  // namespace ambilight::firmware
