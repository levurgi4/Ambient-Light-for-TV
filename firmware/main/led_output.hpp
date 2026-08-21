#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "driver/rmt_encoder.h"
#include "driver/rmt_types.h"
#include "esp_err.h"

#include "firmware_types.hpp"
#include "led_output_config.hpp"

namespace ambilight::firmware {

class LedOutput {
public:
    virtual ~LedOutput() = default;

    [[nodiscard]] virtual esp_err_t initialize(const DeviceConfig& config) = 0;
    [[nodiscard]] virtual esp_err_t reconfigure(const DeviceConfig& config) = 0;
    [[nodiscard]] virtual esp_err_t show(std::span<const Rgb> pixels) = 0;
    [[nodiscard]] virtual bool has_physical_output() const noexcept = 0;
    [[nodiscard]] virtual bool is_ready() const noexcept = 0;
    virtual void shutdown() = 0;
};

// Safe default backend: it validates sizes but never configures a GPIO or
// energizes a strip.
class NoopLedOutput final : public LedOutput {
public:
    [[nodiscard]] esp_err_t initialize(const DeviceConfig& config) override;
    [[nodiscard]] esp_err_t reconfigure(const DeviceConfig& config) override;
    [[nodiscard]] esp_err_t show(std::span<const Rgb> pixels) override;
    [[nodiscard]] bool has_physical_output() const noexcept override;
    [[nodiscard]] bool is_ready() const noexcept override;
    void shutdown() override;

private:
    std::size_t expected_pixel_count_{};
};

// ESP-IDF RMT output for the WS2815 800 kbit/s single-wire protocol. Constructing
// the object does not touch the GPIO. initialize() validates both the device
// configuration and the conservative ESP32-WROOM-32D GPIO allow-list before it
// allocates or enables any peripheral.
class RmtLedOutput final : public LedOutput {
public:
    explicit RmtLedOutput(
        int data_gpio,
        Ws2815TimingProfile timing_profile);
    ~RmtLedOutput() override;

    RmtLedOutput(const RmtLedOutput&) = delete;
    RmtLedOutput& operator=(const RmtLedOutput&) = delete;

    [[nodiscard]] esp_err_t initialize(const DeviceConfig& config) override;
    [[nodiscard]] esp_err_t reconfigure(const DeviceConfig& config) override;
    [[nodiscard]] esp_err_t show(std::span<const Rgb> pixels) override;
    [[nodiscard]] bool has_physical_output() const noexcept override;
    [[nodiscard]] bool is_ready() const noexcept override;
    void shutdown() override;

private:
    void release_hardware();

    int data_gpio_;
    Ws2815Timing timing_;
    rmt_channel_handle_t channel_{};
    rmt_encoder_handle_t encoder_{};
    std::vector<std::uint8_t> wire_bytes_;
    std::vector<Rgb> last_pixels_;
    std::size_t expected_pixel_count_{};
    ColorOrder color_order_{ColorOrder::rgb};
    std::int64_t last_transmission_microseconds_{};
    bool initialized_{};
    bool last_frame_valid_{};
};

}  // namespace ambilight::firmware
