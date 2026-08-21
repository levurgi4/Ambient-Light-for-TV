#include "effects_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "frame_pacing.hpp"
#include "output_limits.hpp"
#include "pixel_mapping.hpp"

namespace ambilight::firmware {
namespace {

constexpr std::uint32_t kRenderFramesPerSecond = 60;
constexpr std::uint32_t kNominalEffectCycleMilliseconds = 3000;
constexpr std::uint64_t kOutputRecoveryIntervalMilliseconds = 1000;

[[nodiscard]] std::uint64_t milliseconds_since_boot() {
    return static_cast<std::uint64_t>(esp_timer_get_time()) / 1000ULL;
}

}  // namespace

EffectsEngine::EffectsEngine(DeviceState& state, LedOutput& output)
    : state_(state), output_(output), pixels_(kDefaultLedCount) {}

bool EffectsEngine::start() {
    return xTaskCreatePinnedToCore(
               task_entry,
               "led_render",
               4096,
               this,
               8,
               nullptr,
               1) == pdPASS;
}

void EffectsEngine::task_entry(void* context) {
    static_cast<EffectsEngine*>(context)->run();
}

void EffectsEngine::run() {
    RenderSnapshot snapshot;
    TickType_t last_wake_time = xTaskGetTickCount();
    static_assert(
        configTICK_RATE_HZ >= kRenderFramesPerSecond,
        "FreeRTOS tick rate is too low for 60 FPS rendering");
    FractionalTickPacer pacer(
        configTICK_RATE_HZ,
        kRenderFramesPerSecond);
    const bool physical_output_present =
        output_.has_physical_output();
    output_faulted_ =
        physical_output_present && !output_.is_ready();
    state_.set_output_status(
        physical_output_present,
        physical_output_present && !output_faulted_);

    for (;;) {
        const auto now = milliseconds_since_boot();
        state_.snapshot_for_render(now, snapshot);

        bool failure_recorded = false;
        if (pixels_.size() != snapshot.config.led_count) {
            pixels_.assign(snapshot.config.led_count, {});
        }

        if (physical_output_present &&
            output_faulted_ &&
            now >= next_output_recovery_at_milliseconds_) {
            // Tear down the complete channel/encoder pair before retrying.
            // Attempts are rate-limited so a persistent hardware fault cannot
            // turn the 60 Hz render task into a tight allocation loop.
            output_.shutdown();
            if (output_.initialize(snapshot.config) == ESP_OK) {
                active_config_ = snapshot.config;
                output_faulted_ = false;
                state_.set_output_status(true, true);
            } else {
                next_output_recovery_at_milliseconds_ =
                    now + kOutputRecoveryIntervalMilliseconds;
                state_.mark_frame_output_failed(
                    snapshot.ambient_sequence,
                    snapshot.generation);
                failure_recorded = true;
            }
        }

        const bool hardware_configuration_changed =
            snapshot.config.led_count != active_config_.led_count ||
            snapshot.config.color_order != active_config_.color_order;
        if (physical_output_present &&
            !output_faulted_ &&
            hardware_configuration_changed) {
            if (output_.reconfigure(snapshot.config) == ESP_OK) {
                active_config_ = snapshot.config;
            } else {
                output_faulted_ = true;
                next_output_recovery_at_milliseconds_ =
                    now + kOutputRecoveryIntervalMilliseconds;
                state_.mark_frame_output_failed(
                    snapshot.ambient_sequence,
                    snapshot.generation);
                failure_recorded = true;
            }
        } else if (!hardware_configuration_changed) {
            // Mapping and limiter fields affect rendering only; they must not
            // clear/reallocate the RMT output.
            active_config_ = snapshot.config;
        }

        render(snapshot, now);
        apply_output_limits(
            pixels_,
            snapshot.config.color_calibration,
            snapshot.config.brightness_limit,
            snapshot.config.current_limit_milliamps);

        const bool output_configuration_ready =
            physical_output_present &&
            !output_faulted_ &&
            output_.is_ready() &&
            snapshot.config.led_count == active_config_.led_count &&
            snapshot.config.color_order == active_config_.color_order;
        if (output_configuration_ready) {
            if (output_.show(pixels_) == ESP_OK) {
                if (snapshot.mode == OperatingMode::ambient) {
                    state_.mark_frame_applied(
                        snapshot.ambient_sequence,
                        snapshot.generation);
                }
            } else {
                output_faulted_ = true;
                next_output_recovery_at_milliseconds_ =
                    now + kOutputRecoveryIntervalMilliseconds;
                state_.mark_frame_output_failed(
                    snapshot.ambient_sequence,
                    snapshot.generation);
                failure_recorded = true;
            }
        } else if (!failure_recorded &&
                   snapshot.mode == OperatingMode::ambient &&
                   snapshot.ambient_frame_claimed) {
            state_.mark_frame_output_failed(
                snapshot.ambient_sequence,
                snapshot.generation);
        }

        const auto frame_period = static_cast<TickType_t>(
            pacer.next_interval());
        const auto now_ticks = xTaskGetTickCount();
        const auto deadline = last_wake_time + frame_period;
        if (static_cast<std::int32_t>(deadline - now_ticks) > 0) {
            vTaskDelayUntil(&last_wake_time, frame_period);
        } else {
            // Drop a missed scheduling slot instead of running several frames
            // back-to-back to catch up after a long RMT/UART/OS stall.
            last_wake_time = now_ticks;
            pacer.reset_phase();
        }
    }
}

void EffectsEngine::render(
    const RenderSnapshot& snapshot,
    const std::uint64_t now_milliseconds) {
    const auto count = pixels_.size();
    if (count == 0) {
        return;
    }

    switch (snapshot.mode) {
        case OperatingMode::off:
            std::fill(pixels_.begin(), pixels_.end(), Rgb{});
            break;

        case OperatingMode::ambient:
            if (snapshot.ambient_pixels.size() == count) {
                std::copy(
                    snapshot.ambient_pixels.begin(),
                    snapshot.ambient_pixels.end(),
                    pixels_.begin());
            } else {
                std::fill(pixels_.begin(), pixels_.end(), Rgb{});
            }
            break;

        case OperatingMode::solid:
            std::fill(
                pixels_.begin(),
                pixels_.end(),
                scale(
                    snapshot.effect.color,
                    snapshot.effect.intensity));
            break;

        case OperatingMode::breathing: {
            const auto cycle =
                kNominalEffectCycleMilliseconds * 100U /
                snapshot.effect.speed_percent;
            const auto phase = static_cast<std::uint8_t>(
                (now_milliseconds % cycle) * 256ULL / cycle);
            const auto level = triangle_wave(phase);
            std::fill(
                pixels_.begin(),
                pixels_.end(),
                scale(
                    scale(
                        snapshot.effect.color,
                        snapshot.effect.intensity),
                    level));
            break;
        }

        case OperatingMode::wave: {
            const auto temporal_phase = static_cast<std::uint8_t>(
                now_milliseconds * snapshot.effect.speed_percent / 1200U);
            for (std::size_t index = 0; index < count; ++index) {
                const auto logical_index = logical_index_for_physical(
                    index,
                    count,
                    snapshot.config.start_offset,
                    snapshot.config.reversed);
                const auto spatial_phase = static_cast<std::uint8_t>(
                    logical_index * 256U / count);
                const auto level = triangle_wave(
                    static_cast<std::uint8_t>(
                        temporal_phase + spatial_phase));
                pixels_[index] = scale(
                    scale(
                        snapshot.effect.color,
                        snapshot.effect.intensity),
                    level);
            }
            break;
        }

        case OperatingMode::rainbow: {
            const auto temporal_hue = static_cast<std::uint8_t>(
                now_milliseconds * snapshot.effect.speed_percent / 1600U);
            for (std::size_t index = 0; index < count; ++index) {
                const auto logical_index = logical_index_for_physical(
                    index,
                    count,
                    snapshot.config.start_offset,
                    snapshot.config.reversed);
                const auto spatial_hue = static_cast<std::uint8_t>(
                    logical_index * 256U / count);
                pixels_[index] = scale(
                    hue_to_rgb(
                        static_cast<std::uint8_t>(
                            temporal_hue + spatial_hue)),
                    snapshot.effect.intensity);
            }
            break;
        }
    }
}

Rgb EffectsEngine::hue_to_rgb(const std::uint8_t hue) {
    const auto region = static_cast<std::uint8_t>(hue / 43U);
    const auto remainder = static_cast<std::uint8_t>(
        (hue - region * 43U) * 6U);
    const auto rising = remainder;
    const auto falling = static_cast<std::uint8_t>(255U - remainder);

    switch (region) {
        case 0:
            return {255, rising, 0};
        case 1:
            return {falling, 255, 0};
        case 2:
            return {0, 255, rising};
        case 3:
            return {0, falling, 255};
        case 4:
            return {rising, 0, 255};
        default:
            return {255, 0, falling};
    }
}

std::uint8_t EffectsEngine::triangle_wave(const std::uint8_t phase) {
    const auto doubled = static_cast<std::uint16_t>(phase) * 2U;
    const auto value =
        doubled <= 255U ? doubled : 510U - doubled;
    // A small floor keeps decorative effects visible at the dark end.
    return static_cast<std::uint8_t>(32U + value * 223U / 255U);
}

Rgb EffectsEngine::scale(const Rgb color, const std::uint8_t factor) {
    const auto scale_channel = [factor](const std::uint8_t channel) {
        return static_cast<std::uint8_t>(
            (static_cast<std::uint16_t>(channel) * factor + 127U) / 255U);
    };
    return {
        scale_channel(color.red),
        scale_channel(color.green),
        scale_channel(color.blue),
    };
}

}  // namespace ambilight::firmware
