#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ambient_frame_lifecycle.hpp"
#include "firmware_types.hpp"

namespace ambilight::firmware {

struct RenderSnapshot {
    DeviceConfig config{};
    EffectSettings effect{};
    OperatingMode mode{OperatingMode::off};
    std::vector<Rgb> ambient_pixels{};
    std::uint32_t ambient_sequence{};
    std::uint32_t generation{};
    bool ambient_frame_claimed{};
};

class DeviceState final {
public:
    DeviceState();
    ~DeviceState();

    DeviceState(const DeviceState&) = delete;
    DeviceState& operator=(const DeviceState&) = delete;

    [[nodiscard]] bool set_config(const DeviceConfig& config);
    [[nodiscard]] bool set_brightness(std::uint8_t brightness);
    [[nodiscard]] bool set_mode(OperatingMode mode, const EffectSettings& effect);
    [[nodiscard]] bool accept_ambient_frame(
        std::uint32_t sequence,
        std::span<const Rgb> pixels,
        std::uint64_t received_at_milliseconds);

    void snapshot_for_render(
        std::uint64_t now_milliseconds,
        RenderSnapshot& destination);
    void mark_frame_applied(
        std::uint32_t sequence,
        std::uint32_t generation);
    void mark_frame_output_failed(
        std::uint32_t sequence,
        std::uint32_t generation);
    void set_output_status(bool physical_output_present, bool output_healthy);
    void mark_packet_rejected();
    void begin_host_session();

    [[nodiscard]] DeviceConfig config() const;
    [[nodiscard]] DeviceStatus status(std::uint64_t now_milliseconds) const;

private:
    class LockGuard final {
    public:
        explicit LockGuard(SemaphoreHandle_t mutex);
        ~LockGuard();

        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;

    private:
        SemaphoreHandle_t mutex_;
    };

    mutable SemaphoreHandle_t mutex_{};
    DeviceConfig config_{};
    EffectSettings effect_{};
    OperatingMode mode_{OperatingMode::off};
    OperatingMode selected_effect_mode_{OperatingMode::breathing};
    EffectSettings fallback_effect_{};
    OperatingMode fallback_mode_{OperatingMode::off};
    std::vector<Rgb> ambient_pixels_;
    std::uint64_t last_ambient_frame_at_milliseconds_{};
    bool has_ambient_frame_{};
    AmbientFrameLifecycle ambient_frames_{};
    std::uint32_t render_generation_{1};
    DeviceStatus status_{};
};

}  // namespace ambilight::firmware
