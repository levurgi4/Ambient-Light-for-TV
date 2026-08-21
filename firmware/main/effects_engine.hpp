#pragma once

#include <cstdint>
#include <vector>

#include "device_state.hpp"
#include "led_output.hpp"

namespace ambilight::firmware {

class EffectsEngine final {
public:
    EffectsEngine(DeviceState& state, LedOutput& output);

    [[nodiscard]] bool start();

private:
    static void task_entry(void* context);
    void run();
    void render(const RenderSnapshot& snapshot, std::uint64_t now_milliseconds);

    [[nodiscard]] static Rgb hue_to_rgb(std::uint8_t hue);
    [[nodiscard]] static std::uint8_t triangle_wave(std::uint8_t phase);
    [[nodiscard]] static Rgb scale(Rgb color, std::uint8_t factor);

    DeviceState& state_;
    LedOutput& output_;
    std::vector<Rgb> pixels_;
    DeviceConfig active_config_{};
    std::uint64_t next_output_recovery_at_milliseconds_{};
    bool output_faulted_{};
};

}  // namespace ambilight::firmware
