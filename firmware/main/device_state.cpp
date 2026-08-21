#include "device_state.hpp"

#include <algorithm>

#include "esp_system.h"

#include "ambient_lease.hpp"

namespace ambilight::firmware {

DeviceState::LockGuard::LockGuard(const SemaphoreHandle_t mutex) : mutex_(mutex) {
    if (mutex_ == nullptr || xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        abort();
    }
}

DeviceState::LockGuard::~LockGuard() {
    xSemaphoreGive(mutex_);
}

DeviceState::DeviceState()
    : mutex_(xSemaphoreCreateMutex()),
      ambient_pixels_(config_.led_count),
      status_{
          .mode = OperatingMode::off,
          .selected_effect_mode = selected_effect_mode_,
          .effect = effect_,
          .led_count = config_.led_count,
      } {
    if (mutex_ == nullptr) {
        abort();
    }
}

DeviceState::~DeviceState() {
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
    }
}

bool DeviceState::set_config(const DeviceConfig& config) {
    if (!is_valid(config)) {
        return false;
    }

    {
        const LockGuard lock(mutex_);
        // ACK retries are allowed to repeat a command after its response was
        // lost. An identical SetStripConfig must not invalidate a queued frame,
        // reset sequences, or advance the render generation.
        if (!requires_config_update(config_, config)) {
            return true;
        }
        if (!requires_ambient_frame_invalidation(config_, config)) {
            config_ = config;
            status_.led_count = config.led_count;
            return true;
        }
    }

    std::vector<Rgb> resized_pixels(config.led_count);

    const LockGuard lock(mutex_);
    // Recheck after allocating outside the mutex in case another writer
    // completed the same update meanwhile.
    if (!requires_config_update(config_, config)) {
        return true;
    }
    if (!requires_ambient_frame_invalidation(config_, config)) {
        config_ = config;
        status_.led_count = config.led_count;
        return true;
    }
    if (ambient_frames_.discard_queued()) {
        ++status_.dropped_frames;
    }
    ambient_pixels_.swap(resized_pixels);
    config_ = config;
    has_ambient_frame_ = false;
    last_ambient_frame_at_milliseconds_ = 0;
    ++render_generation_;
    status_.last_received_frame_sequence = 0;
    status_.last_applied_frame_sequence = 0;
    status_.led_count = config.led_count;
    return true;
}

bool DeviceState::set_brightness(const std::uint8_t brightness) {
    const LockGuard lock(mutex_);
    config_.brightness_limit = brightness;
    return true;
}

bool DeviceState::set_mode(
    const OperatingMode mode,
    const EffectSettings& effect) {
    if (!is_valid(effect)) {
        return false;
    }

    switch (mode) {
        case OperatingMode::off:
        case OperatingMode::ambient:
        case OperatingMode::solid:
        case OperatingMode::breathing:
        case OperatingMode::wave:
        case OperatingMode::rainbow:
            break;
        default:
            return false;
    }

    const LockGuard lock(mutex_);
    if ((mode_ == OperatingMode::ambient)
        != (mode == OperatingMode::ambient)) {
        if (mode_ == OperatingMode::ambient &&
            ambient_frames_.discard_queued()) {
            ++status_.dropped_frames;
        }
        has_ambient_frame_ = false;
        last_ambient_frame_at_milliseconds_ = 0;
        ++render_generation_;
        status_.last_received_frame_sequence = 0;
        status_.last_applied_frame_sequence = 0;
    }
    mode_ = mode;
    effect_ = effect;
    if (mode == OperatingMode::solid ||
        mode == OperatingMode::breathing ||
        mode == OperatingMode::wave ||
        mode == OperatingMode::rainbow) {
        selected_effect_mode_ = mode;
    }
    if (mode != OperatingMode::ambient) {
        fallback_mode_ = mode;
        fallback_effect_ = effect;
    }
    status_.mode = mode;
    status_.selected_effect_mode = selected_effect_mode_;
    status_.effect = effect;
    return true;
}

bool DeviceState::accept_ambient_frame(
    const std::uint32_t sequence,
    const std::span<const Rgb> pixels,
    const std::uint64_t received_at_milliseconds) {
    const LockGuard lock(mutex_);
    if (mode_ != OperatingMode::ambient) {
        return false;
    }
    if (pixels.size() != ambient_pixels_.size()) {
        ++status_.dropped_frames;
        return false;
    }

    const auto sequence_delta = static_cast<std::int32_t>(
        sequence - status_.last_received_frame_sequence);
    if (has_ambient_frame_ && sequence_delta <= 0) {
        ++status_.dropped_frames;
        return false;
    }

    if (ambient_frames_.enqueue()) {
        ++status_.dropped_frames;
    }

    std::copy(pixels.begin(), pixels.end(), ambient_pixels_.begin());
    last_ambient_frame_at_milliseconds_ = received_at_milliseconds;
    has_ambient_frame_ = true;
    status_.last_received_frame_sequence = sequence;
    return true;
}

void DeviceState::snapshot_for_render(
    const std::uint64_t now_milliseconds,
    RenderSnapshot& destination) {
    const LockGuard lock(mutex_);
    destination.config = config_;
    destination.effect = effect_;
    destination.mode = mode_;
    destination.ambient_sequence = status_.last_received_frame_sequence;
    destination.generation = render_generation_;
    destination.ambient_frame_claimed = false;

    const bool ambient_timed_out =
        mode_ == OperatingMode::ambient &&
        !is_ambient_lease_active(
            has_ambient_frame_,
            now_milliseconds,
            last_ambient_frame_at_milliseconds_,
            config_.ambient_timeout_milliseconds);

    if (ambient_timed_out) {
        destination.mode = fallback_mode_;
        destination.effect = fallback_effect_;
    }

    if (destination.mode == OperatingMode::ambient) {
        destination.ambient_pixels = ambient_pixels_;
        destination.ambient_frame_claimed = ambient_frames_.claim(
            destination.ambient_sequence,
            destination.generation);
    } else {
        if (ambient_timed_out &&
            ambient_frames_.discard_queued()) {
            ++status_.dropped_frames;
        }
        destination.ambient_pixels.clear();
    }
}

void DeviceState::mark_frame_applied(
    const std::uint32_t sequence,
    const std::uint32_t generation) {
    const LockGuard lock(mutex_);
    if (!ambient_frames_.applied(sequence, generation)) {
        return;
    }
    if (generation != render_generation_
        || mode_ != OperatingMode::ambient
        || !has_ambient_frame_) {
        return;
    }
    status_.last_applied_frame_sequence = sequence;
}

void DeviceState::mark_frame_output_failed(
    const std::uint32_t sequence,
    const std::uint32_t generation) {
    const LockGuard lock(mutex_);
    status_.output_healthy = false;
    const bool still_relevant =
        generation == render_generation_ &&
        mode_ == OperatingMode::ambient &&
        has_ambient_frame_;
    if (ambient_frames_.failed(
            sequence,
            generation,
            still_relevant) == AmbientFailureResolution::dropped) {
        ++status_.dropped_frames;
    }
}

void DeviceState::set_output_status(
    const bool physical_output_present,
    const bool output_healthy) {
    const LockGuard lock(mutex_);
    status_.physical_output_present = physical_output_present;
    status_.output_healthy =
        physical_output_present && output_healthy;
}

void DeviceState::mark_packet_rejected() {
    const LockGuard lock(mutex_);
    ++status_.rejected_packets;
}

void DeviceState::begin_host_session() {
    const LockGuard lock(mutex_);
    if (ambient_frames_.discard_queued()) {
        ++status_.dropped_frames;
    }
    last_ambient_frame_at_milliseconds_ = 0;
    has_ambient_frame_ = false;
    ++render_generation_;
    status_.last_received_frame_sequence = 0;
    status_.last_applied_frame_sequence = 0;
}

DeviceConfig DeviceState::config() const {
    const LockGuard lock(mutex_);
    return config_;
}

DeviceStatus DeviceState::status(const std::uint64_t now_milliseconds) const {
    const LockGuard lock(mutex_);
    auto result = status_;
    result.ambient_lease_active =
        mode_ == OperatingMode::ambient &&
        is_ambient_lease_active(
            has_ambient_frame_,
            now_milliseconds,
            last_ambient_frame_at_milliseconds_,
            config_.ambient_timeout_milliseconds);
    if (mode_ == OperatingMode::ambient && !result.ambient_lease_active) {
        result.mode = fallback_mode_;
        result.effect = fallback_effect_;
    }
    return result;
}

}  // namespace ambilight::firmware
