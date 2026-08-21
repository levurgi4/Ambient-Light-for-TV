#include "led_output.hpp"

#include <algorithm>
#include <cstdlib>

#include "driver/gpio.h"
#include "driver/rmt_common.h"
#include "driver/rmt_tx.h"
#include "esp_timer.h"

#include "led_output_config.hpp"
#include "led_wire_format.hpp"

namespace ambilight::firmware {
namespace {

constexpr int kTransmissionTimeoutMilliseconds = 20;
constexpr std::int64_t kStaticFrameRelatchMicroseconds = 1'000'000;

[[nodiscard]] constexpr std::uint32_t pack_rmt_symbol(
    const std::uint16_t duration0,
    const bool level0,
    const std::uint16_t duration1,
    const bool level1) noexcept {
    // rmt_symbol_word_t exposes 15-bit duration bit-fields. Pack through its
    // uint32_t representation to avoid an implementation-defined narrowing
    // assignment while retaining the hardware-defined layout.
    return (static_cast<std::uint32_t>(duration0) & 0x7FFFU) |
           (static_cast<std::uint32_t>(level0) << 15U) |
           ((static_cast<std::uint32_t>(duration1) & 0x7FFFU) << 16U) |
           (static_cast<std::uint32_t>(level1) << 31U);
}

enum class EncoderPhase : std::uint8_t {
    bytes,
    reset,
};

struct Ws2815Encoder {
    rmt_encoder_t base{};
    rmt_encoder_handle_t bytes_encoder{};
    rmt_encoder_handle_t copy_encoder{};
    EncoderPhase phase{EncoderPhase::bytes};
    rmt_symbol_word_t reset_symbol{};
};

[[nodiscard]] Ws2815Encoder* as_ws2815_encoder(
    rmt_encoder_t* encoder) noexcept {
    // base is deliberately the first member, matching the ESP-IDF custom
    // encoder pattern.
    return reinterpret_cast<Ws2815Encoder*>(encoder);
}

RMT_ENCODER_FUNC_ATTR
std::size_t encode_ws2815(
    rmt_encoder_t* encoder,
    const rmt_channel_handle_t channel,
    const void* primary_data,
    const std::size_t data_size,
    rmt_encode_state_t* return_state) {
    auto* const ws2815 = as_ws2815_encoder(encoder);
    std::size_t encoded_symbols = 0;
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;

    if (ws2815->phase == EncoderPhase::bytes) {
        encoded_symbols += ws2815->bytes_encoder->encode(
            ws2815->bytes_encoder,
            channel,
            primary_data,
            data_size,
            &session_state);
        if ((session_state & RMT_ENCODING_COMPLETE) != 0) {
            ws2815->phase = EncoderPhase::reset;
        }
        if ((session_state & RMT_ENCODING_MEM_FULL) != 0) {
            *return_state = RMT_ENCODING_MEM_FULL;
            return encoded_symbols;
        }
        if (ws2815->phase == EncoderPhase::bytes) {
            *return_state = RMT_ENCODING_RESET;
            return encoded_symbols;
        }
    }

    session_state = RMT_ENCODING_RESET;
    encoded_symbols += ws2815->copy_encoder->encode(
        ws2815->copy_encoder,
        channel,
        &ws2815->reset_symbol,
        sizeof(ws2815->reset_symbol),
        &session_state);

    if ((session_state & RMT_ENCODING_COMPLETE) != 0) {
        ws2815->phase = EncoderPhase::bytes;
    }
    // Preserve both flags when the child encoder completes exactly as its RMT
    // memory becomes full. Dropping MEM_FULL is harmless with today's 64
    // symbols but would make the custom encoder fragile if that size changes.
    *return_state = session_state;
    return encoded_symbols;
}

esp_err_t reset_ws2815_encoder(rmt_encoder_t* encoder) {
    auto* const ws2815 = as_ws2815_encoder(encoder);
    const auto bytes_result = rmt_encoder_reset(ws2815->bytes_encoder);
    const auto copy_result = rmt_encoder_reset(ws2815->copy_encoder);
    ws2815->phase = EncoderPhase::bytes;
    return bytes_result != ESP_OK ? bytes_result : copy_result;
}

esp_err_t delete_ws2815_encoder(rmt_encoder_t* encoder) {
    auto* const ws2815 = as_ws2815_encoder(encoder);
    auto result = ESP_OK;
    if (ws2815->bytes_encoder != nullptr) {
        const auto bytes_result =
            rmt_del_encoder(ws2815->bytes_encoder);
        if (bytes_result == ESP_OK) {
            ws2815->bytes_encoder = nullptr;
        } else {
            result = bytes_result;
        }
    }
    if (ws2815->copy_encoder != nullptr) {
        const auto copy_result = rmt_del_encoder(ws2815->copy_encoder);
        if (copy_result == ESP_OK) {
            ws2815->copy_encoder = nullptr;
        } else if (result == ESP_OK) {
            result = copy_result;
        }
    }
    // ESP-IDF keeps the parent handle when del() reports an error. Preserve a
    // still-valid parent as well, so release_hardware() can retry only the
    // child encoder(s) that refused deletion instead of retaining a dangling
    // encoder_ pointer.
    if (ws2815->bytes_encoder == nullptr &&
        ws2815->copy_encoder == nullptr) {
        std::free(ws2815);
    }
    return result;
}

[[nodiscard]] esp_err_t new_ws2815_encoder(
    const Ws2815Timing timing,
    rmt_encoder_handle_t* return_encoder) {
    if (return_encoder == nullptr || !is_valid(timing)) {
        return ESP_ERR_INVALID_ARG;
    }
    *return_encoder = nullptr;

    auto* const encoder = static_cast<Ws2815Encoder*>(
        rmt_alloc_encoder_mem(sizeof(Ws2815Encoder)));
    if (encoder == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    encoder->base.encode = encode_ws2815;
    encoder->base.reset = reset_ws2815_encoder;
    encoder->base.del = delete_ws2815_encoder;
    encoder->phase = EncoderPhase::bytes;
    encoder->reset_symbol = {
        .val = pack_rmt_symbol(
            timing.reset_half_ticks,
            false,
            timing.reset_half_ticks,
            false),
    };

    const rmt_bytes_encoder_config_t bytes_config{
        .bit0 = {
            .val = pack_rmt_symbol(
                timing.zero_high_ticks,
                true,
                timing.zero_low_ticks,
                false),
        },
        .bit1 = {
            .val = pack_rmt_symbol(
                timing.one_high_ticks,
                true,
                timing.one_low_ticks,
                false),
        },
        .flags = {
            .msb_first = 1,
        },
    };
    auto result = rmt_new_bytes_encoder(
        &bytes_config,
        &encoder->bytes_encoder);
    if (result != ESP_OK) {
        std::free(encoder);
        return result;
    }

    constexpr rmt_copy_encoder_config_t copy_config{};
    result = rmt_new_copy_encoder(
        &copy_config,
        &encoder->copy_encoder);
    if (result != ESP_OK) {
        // Transfer ownership of the partially constructed parent to the
        // caller. initialize() immediately routes it through
        // release_hardware(), whose retry-safe del callback retains the parent
        // if deletion of the bytes child fails.
        *return_encoder = &encoder->base;
        return result;
    }

    *return_encoder = &encoder->base;
    return ESP_OK;
}

void hold_gpio_low_safely(const int gpio) {
    if (!is_safe_led_data_gpio(gpio)) {
        return;
    }

    const gpio_config_t safe_gpio_config{
        .pin_bit_mask = 1ULL << static_cast<unsigned int>(gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    static_cast<void>(gpio_config(&safe_gpio_config));
}

}  // namespace

esp_err_t NoopLedOutput::initialize(const DeviceConfig& config) {
    if (!is_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    expected_pixel_count_ = config.led_count;
    return ESP_OK;
}

esp_err_t NoopLedOutput::reconfigure(const DeviceConfig& config) {
    return initialize(config);
}

esp_err_t NoopLedOutput::show(const std::span<const Rgb> pixels) {
    return pixels.size() == expected_pixel_count_
        ? ESP_OK
        : ESP_ERR_INVALID_SIZE;
}

bool NoopLedOutput::has_physical_output() const noexcept {
    return false;
}

bool NoopLedOutput::is_ready() const noexcept {
    return false;
}

void NoopLedOutput::shutdown() {
    expected_pixel_count_ = 0;
}

RmtLedOutput::RmtLedOutput(
    const int data_gpio,
    const Ws2815TimingProfile timing_profile)
    : data_gpio_(data_gpio),
      timing_(ws2815_timing(timing_profile)) {}

RmtLedOutput::~RmtLedOutput() {
    shutdown();
}

esp_err_t RmtLedOutput::initialize(const DeviceConfig& config) {
    if (initialized_ || channel_ != nullptr || encoder_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!is_valid(config) ||
        !is_valid(timing_) ||
        !is_safe_led_data_gpio(data_gpio_)) {
        return ESP_ERR_INVALID_ARG;
    }

    wire_bytes_.assign(
        static_cast<std::size_t>(config.led_count) * kLedBytesPerPixel,
        0);
    last_pixels_.assign(config.led_count, {});

    auto result = new_ws2815_encoder(timing_, &encoder_);
    if (result != ESP_OK) {
        release_hardware();
        return result;
    }

    const rmt_tx_channel_config_t channel_config{
        .gpio_num = static_cast<gpio_num_t>(data_gpio_),
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = timing_.resolution_hz,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
        .intr_priority = 0,
        .flags = {
            .invert_out = 0,
            .with_dma = 0,
            .allow_pd = 0,
            .init_level = 0,
        },
    };
    result = rmt_new_tx_channel(&channel_config, &channel_);
    if (result != ESP_OK) {
        release_hardware();
        return result;
    }

    result = rmt_enable(channel_);
    if (result != ESP_OK) {
        release_hardware();
        return result;
    }

    expected_pixel_count_ = config.led_count;
    color_order_ = config.color_order;
    initialized_ = true;
    last_frame_valid_ = false;
    last_transmission_microseconds_ = 0;

    // A powered strip keeps its previously latched colors across an ESP32
    // reset. Clear it before the render task starts so the startup OFF state is
    // physical, not merely an in-memory mode.
    result = show(last_pixels_);
    if (result != ESP_OK) {
        shutdown();
    }
    return result;
}

esp_err_t RmtLedOutput::reconfigure(const DeviceConfig& config) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!is_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }

    // Clear the complete old physical span before shrinking the configured
    // count. Otherwise LEDs beyond the new end would keep their previously
    // latched colors forever because they no longer receive data.
    const std::vector<Rgb> cleared_pixels(expected_pixel_count_);
    const auto clear_result = show(cleared_pixels);
    if (clear_result != ESP_OK) {
        return clear_result;
    }

    const auto wait_result = rmt_tx_wait_all_done(
        channel_,
        kTransmissionTimeoutMilliseconds);
    if (wait_result != ESP_OK) {
        return wait_result;
    }

    wire_bytes_.assign(
        static_cast<std::size_t>(config.led_count) * kLedBytesPerPixel,
        0);
    last_pixels_.assign(config.led_count, {});
    expected_pixel_count_ = config.led_count;
    color_order_ = config.color_order;
    last_frame_valid_ = false;
    last_transmission_microseconds_ = 0;
    return ESP_OK;
}

esp_err_t RmtLedOutput::show(const std::span<const Rgb> pixels) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels.size() != expected_pixel_count_) {
        return ESP_ERR_INVALID_SIZE;
    }
    const auto now_microseconds = esp_timer_get_time();
    if (last_frame_valid_ &&
        std::equal(
            pixels.begin(),
            pixels.end(),
            last_pixels_.begin(),
            last_pixels_.end()) &&
        now_microseconds - last_transmission_microseconds_ <
            kStaticFrameRelatchMicroseconds) {
        return ESP_OK;
    }

    // Never modify the payload while a previous asynchronous RMT transaction
    // could still refer to it.
    auto result = rmt_tx_wait_all_done(
        channel_,
        kTransmissionTimeoutMilliseconds);
    if (result != ESP_OK) {
        return result;
    }
    if (!encode_led_wire_bytes(pixels, color_order_, wire_bytes_)) {
        return ESP_ERR_INVALID_ARG;
    }

    constexpr rmt_transmit_config_t transmit_config{
        .loop_count = 0,
        .flags = {
            .eot_level = 0,
            .queue_nonblocking = 1,
        },
    };
    result = rmt_transmit(
        channel_,
        encoder_,
        wire_bytes_.data(),
        wire_bytes_.size(),
        &transmit_config);
    if (result != ESP_OK) {
        return result;
    }

    result = rmt_tx_wait_all_done(
        channel_,
        kTransmissionTimeoutMilliseconds);
    if (result == ESP_OK) {
        std::copy(
            pixels.begin(),
            pixels.end(),
            last_pixels_.begin());
        last_frame_valid_ = true;
        last_transmission_microseconds_ = esp_timer_get_time();
    }
    return result;
}

bool RmtLedOutput::has_physical_output() const noexcept {
    return true;
}

bool RmtLedOutput::is_ready() const noexcept {
    return initialized_;
}

void RmtLedOutput::shutdown() {
    release_hardware();
    initialized_ = false;
    last_frame_valid_ = false;
    last_transmission_microseconds_ = 0;
    // A timed-out transaction may still refer to wire_bytes_. Do not release
    // its storage until the RMT channel has actually accepted deletion.
    if (channel_ == nullptr) {
        wire_bytes_.clear();
        last_pixels_.clear();
        expected_pixel_count_ = 0;
    }
}

void RmtLedOutput::release_hardware() {
    const bool gpio_was_claimed = channel_ != nullptr;
    if (channel_ != nullptr) {
        const auto wait_result = rmt_tx_wait_all_done(
            channel_,
            kTransmissionTimeoutMilliseconds);
        if (wait_result != ESP_OK) {
            // On the original ESP32, rmt_disable() terminates a running
            // transaction with an internal, unbounded busy-wait for TX_DONE.
            // Keep the live channel and payload intact instead; the engine will
            // make another bounded teardown attempt on its next recovery tick.
            return;
        }
        static_cast<void>(rmt_disable(channel_));
        if (rmt_del_channel(channel_) == ESP_OK) {
            channel_ = nullptr;
        }
    }
    // The encoder can still be referenced by an in-flight transaction if the
    // channel refused deletion. Retain both handles and retry the teardown on
    // the next bounded recovery attempt instead of leaking/reallocating RMT.
    if (channel_ == nullptr && encoder_ != nullptr) {
        if (rmt_del_encoder(encoder_) == ESP_OK) {
            encoder_ = nullptr;
        }
    }
    if (gpio_was_claimed && channel_ == nullptr) {
        hold_gpio_low_safely(data_gpio_);
    }
}

}  // namespace ambilight::firmware
