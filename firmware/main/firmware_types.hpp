#pragma once

#include <cstddef>
#include <cstdint>

namespace ambilight::firmware {

inline constexpr std::uint16_t kDefaultLedCount = 300;
inline constexpr std::uint16_t kDefaultTopLedCount = 96;
inline constexpr std::uint16_t kDefaultRightLedCount = 54;
inline constexpr std::uint16_t kDefaultBottomLedCount = 96;
inline constexpr std::uint16_t kDefaultLeftLedCount = 54;
inline constexpr std::uint16_t kMaximumLedCount = 300;
inline constexpr std::uint8_t kDefaultBrightnessLimit = 128;
inline constexpr std::uint16_t kDefaultCurrentLimitMilliamps = 5000;
inline constexpr std::uint16_t kDefaultAmbientTimeoutMilliseconds = 1000;
inline constexpr std::uint16_t kDefaultEffectSpeedPercent = 100;
inline constexpr std::uint32_t kEstimatedIdleMilliampsPerPixel = 3;

struct Rgb {
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};

    [[nodiscard]] constexpr bool operator==(const Rgb&) const = default;
};

enum class StripType : std::uint8_t {
    ws2815 = 0,
};

enum class ColorOrder : std::uint8_t {
    rgb = 0,
    rbg,
    grb,
    gbr,
    brg,
    bgr,
};

[[nodiscard]] constexpr bool is_valid(const ColorOrder order) noexcept {
    switch (order) {
        case ColorOrder::rgb:
        case ColorOrder::rbg:
        case ColorOrder::grb:
        case ColorOrder::gbr:
        case ColorOrder::brg:
        case ColorOrder::bgr:
            return true;
    }
    return false;
}

enum class OperatingMode : std::uint8_t {
    off = 0,
    ambient,
    solid,
    breathing,
    wave,
    rainbow,
};

struct DeviceConfig {
    StripType strip_type{StripType::ws2815};
    ColorOrder color_order{ColorOrder::rgb};
    std::uint16_t led_count{kDefaultLedCount};
    std::uint16_t top_count{kDefaultTopLedCount};
    std::uint16_t right_count{kDefaultRightLedCount};
    std::uint16_t bottom_count{kDefaultBottomLedCount};
    std::uint16_t left_count{kDefaultLeftLedCount};
    std::uint16_t start_offset{};
    std::uint8_t brightness_limit{kDefaultBrightnessLimit};
    std::uint16_t current_limit_milliamps{kDefaultCurrentLimitMilliamps};
    std::uint16_t ambient_timeout_milliseconds{kDefaultAmbientTimeoutMilliseconds};
    Rgb color_calibration{255, 255, 255};
    bool reversed{};

    [[nodiscard]] constexpr bool operator==(
        const DeviceConfig&) const = default;
};

struct EffectSettings {
    Rgb color{255, 96, 24};
    std::uint8_t intensity{255};
    std::uint16_t speed_percent{kDefaultEffectSpeedPercent};
};

[[nodiscard]] constexpr bool requires_config_update(
    const DeviceConfig& current,
    const DeviceConfig& requested) noexcept {
    return !(current == requested);
}

// These fields alter the topology or the physical interpretation of a stored
// ambient frame. Rendering limits and the lease timeout can be updated in
// place without discarding pixels that are already queued or displayed.
[[nodiscard]] constexpr bool requires_ambient_frame_invalidation(
    const DeviceConfig& current,
    const DeviceConfig& requested) noexcept {
    return current.strip_type != requested.strip_type ||
           current.color_order != requested.color_order ||
           current.led_count != requested.led_count ||
           current.top_count != requested.top_count ||
           current.right_count != requested.right_count ||
           current.bottom_count != requested.bottom_count ||
           current.left_count != requested.left_count ||
           current.start_offset != requested.start_offset ||
           current.reversed != requested.reversed;
}

struct DeviceStatus {
    OperatingMode mode{OperatingMode::off};
    OperatingMode selected_effect_mode{OperatingMode::breathing};
    EffectSettings effect{};
    std::uint16_t led_count{kDefaultLedCount};
    std::uint32_t last_received_frame_sequence{};
    std::uint32_t last_applied_frame_sequence{};
    std::uint32_t dropped_frames{};
    std::uint32_t rejected_packets{};
    bool ambient_lease_active{};
    bool physical_output_present{};
    bool output_healthy{};
};

[[nodiscard]] constexpr bool is_valid(const DeviceConfig& config) noexcept {
    return config.strip_type == StripType::ws2815 &&
           is_valid(config.color_order) &&
           config.led_count > 0 &&
           config.led_count <= kMaximumLedCount &&
           static_cast<std::uint32_t>(config.top_count) +
                   config.right_count +
                   config.bottom_count +
                   config.left_count ==
               config.led_count &&
           config.start_offset < config.led_count &&
           config.current_limit_milliamps >=
               static_cast<std::uint32_t>(config.led_count) *
                   kEstimatedIdleMilliampsPerPixel &&
           config.current_limit_milliamps <= 10'000 &&
           config.ambient_timeout_milliseconds >= 100 &&
           config.ambient_timeout_milliseconds <= 60'000;
}

[[nodiscard]] constexpr bool is_valid(const EffectSettings& settings) noexcept {
    return settings.speed_percent >= 1 && settings.speed_percent <= 1000;
}

}  // namespace ambilight::firmware
