#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#include "ambient_lease.hpp"
#include "firmware_types.hpp"
#include "frame_pacing.hpp"
#include "led_output_config.hpp"
#include "led_wire_format.hpp"
#include "pixel_mapping.hpp"
#include "uart_rx_drain.hpp"

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_all_color_orders() {
    using ambilight::firmware::ColorOrder;
    using ambilight::firmware::Rgb;

    constexpr std::array pixels{Rgb{1, 2, 3}, Rgb{4, 5, 6}};
    struct Case {
        ColorOrder order;
        std::array<std::uint8_t, 6> expected;
    };
    constexpr std::array cases{
        Case{ColorOrder::rgb, {1, 2, 3, 4, 5, 6}},
        Case{ColorOrder::rbg, {1, 3, 2, 4, 6, 5}},
        Case{ColorOrder::grb, {2, 1, 3, 5, 4, 6}},
        Case{ColorOrder::gbr, {2, 3, 1, 5, 6, 4}},
        Case{ColorOrder::brg, {3, 1, 2, 6, 4, 5}},
        Case{ColorOrder::bgr, {3, 2, 1, 6, 5, 4}},
    };

    for (const auto& test_case : cases) {
        std::array<std::uint8_t, 6> encoded{};
        expect(
            ambilight::firmware::encode_led_wire_bytes(
                pixels,
                test_case.order,
                encoded),
            "a supported color order must encode");
        expect(
            encoded == test_case.expected,
            "encoded channel order must match the selected order");
    }
}

void test_rejects_invalid_arguments() {
    using ambilight::firmware::ColorOrder;
    using ambilight::firmware::Rgb;

    constexpr std::array pixels{Rgb{1, 2, 3}};
    std::array<std::uint8_t, 2> short_destination{};
    expect(
        !ambilight::firmware::encode_led_wire_bytes(
            pixels,
            ColorOrder::grb,
            short_destination),
        "a short destination must be rejected");

    std::array<std::uint8_t, 3> destination{};
    expect(
        !ambilight::firmware::encode_led_wire_bytes(
            pixels,
            static_cast<ColorOrder>(255),
            destination),
        "an unknown color order must be rejected");
}

void test_maximum_strip_size() {
    using ambilight::firmware::ColorOrder;
    using ambilight::firmware::Rgb;
    using ambilight::firmware::kMaximumLedCount;

    std::array<Rgb, kMaximumLedCount> pixels{};
    pixels.front() = {10, 20, 30};
    pixels.back() = {40, 50, 60};
    std::array<
        std::uint8_t,
        kMaximumLedCount * ambilight::firmware::kLedBytesPerPixel>
        encoded{};

    expect(
        ambilight::firmware::encode_led_wire_bytes(
            pixels,
            ColorOrder::grb,
            encoded),
        "the maximum 300-pixel strip must encode");
    expect(
        encoded[0] == 20 && encoded[1] == 10 && encoded[2] == 30,
        "the first maximum-strip pixel must be GRB");
    expect(
        encoded[encoded.size() - 3U] == 50 &&
            encoded[encoded.size() - 2U] == 40 &&
            encoded[encoded.size() - 1U] == 60,
        "the last maximum-strip pixel must be GRB");
}

void test_gpio_allow_list() {
    using ambilight::firmware::is_safe_led_data_gpio;

    for (const int gpio : {16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33}) {
        expect(is_safe_led_data_gpio(gpio), "allowed GPIO must be accepted");
    }
    for (const int gpio : {-1, 0, 1, 2, 3, 5, 6, 11, 12, 15, 34, 39}) {
        expect(!is_safe_led_data_gpio(gpio), "unsafe GPIO must be rejected");
    }
}

void test_device_config_rejects_unknown_color_order() {
    ambilight::firmware::DeviceConfig config{};
    config.color_order =
        static_cast<ambilight::firmware::ColorOrder>(255);
    expect(
        !ambilight::firmware::is_valid(config),
        "device configuration must reject an unknown color order");
}

void test_current_limit_covers_modeled_idle_current() {
    using namespace ambilight::firmware;

    DeviceConfig config{};
    config.current_limit_milliamps = static_cast<std::uint16_t>(
        config.led_count * kEstimatedIdleMilliampsPerPixel);
    expect(
        is_valid(config),
        "the exact modeled idle-current budget must be accepted");

    --config.current_limit_milliamps;
    expect(
        !is_valid(config),
        "a current limit below the strip idle estimate must be rejected");
}

void test_device_config_structural_equality() {
    using namespace ambilight::firmware;

    constexpr DeviceConfig original{};
    constexpr DeviceConfig identical{};
    static_assert(original == identical);
    expect(
        !requires_config_update(original, identical),
        "an exact SetStripConfig ACK retry must compare equal");

    auto changed = original;
    changed.start_offset = 1;
    expect(
        requires_config_update(original, changed),
        "a meaningful strip configuration change must compare unequal");
}

void test_ambient_frame_invalidation_policy() {
    using namespace ambilight::firmware;

    constexpr DeviceConfig original{};

    auto rendering_limits = original;
    rendering_limits.brightness_limit = 64;
    rendering_limits.current_limit_milliamps = 4000;
    rendering_limits.ambient_timeout_milliseconds = 2500;
    rendering_limits.color_calibration = {255, 235, 200};
    expect(
        requires_config_update(original, rendering_limits) &&
            !requires_ambient_frame_invalidation(
                original,
                rendering_limits),
        "calibration, brightness, current limit, and lease timeout must preserve ambient pixels");

    auto topology = original;
    topology.top_count = 95;
    topology.bottom_count = 97;
    expect(
        requires_ambient_frame_invalidation(original, topology),
        "a perimeter topology change must invalidate the ambient frame");

    auto mapping = original;
    mapping.start_offset = 1;
    expect(
        requires_ambient_frame_invalidation(original, mapping),
        "a physical start-offset change must invalidate the ambient frame");

    auto direction = original;
    direction.reversed = true;
    expect(
        requires_ambient_frame_invalidation(original, direction),
        "a physical direction change must invalidate the ambient frame");

    auto wire_format = original;
    wire_format.color_order = ColorOrder::grb;
    expect(
        requires_ambient_frame_invalidation(original, wire_format),
        "a wire color-order change must invalidate the ambient frame");

    auto resized = original;
    resized.led_count = 299;
    resized.top_count = 95;
    expect(
        requires_ambient_frame_invalidation(original, resized),
        "an LED-count change must invalidate and resize the ambient frame");
}

void test_ambient_lease_accepts_boot_timestamp_zero() {
    using ambilight::firmware::is_ambient_lease_active;

    expect(
        is_ambient_lease_active(true, 0, 0, 1000),
        "a real frame received at boot millisecond zero must own the lease");
    expect(
        is_ambient_lease_active(true, 1000, 0, 1000),
        "the lease must remain active at its inclusive timeout boundary");
    expect(
        !is_ambient_lease_active(true, 1001, 0, 1000),
        "the lease must expire immediately after its timeout boundary");
    expect(
        !is_ambient_lease_active(false, 0, 0, 1000),
        "a zero timestamp without an accepted frame is not a lease");
    expect(
        !is_ambient_lease_active(true, 99, 100, 1000),
        "a timestamp from the future must not underflow into an active lease");
}

void test_uart_data_notification_drains_all_buffered_bytes() {
    using ambilight::firmware::drain_available_uart_bytes;

    std::array<std::uint8_t, 2500> source{};
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<std::uint8_t>(index & 0xFFU);
    }

    std::array<std::uint8_t, 128> scratch{};
    std::vector<std::uint8_t> consumed;
    consumed.reserve(source.size());
    std::size_t read_offset = 0;

    // Model one surviving UART_DATA event whose ring buffer also contains
    // bytes from notifications that the full ESP-IDF event queue dropped.
    drain_available_uart_bytes(
        scratch,
        [&](const std::span<std::uint8_t> destination) {
            const auto remaining = source.size() - read_offset;
            const auto count = std::min(remaining, destination.size());
            if (count == 0) {
                return 0;
            }
            std::copy_n(
                source.data() + read_offset,
                count,
                destination.data());
            read_offset += count;
            return static_cast<int>(count);
        },
        [&](const std::span<const std::uint8_t> bytes) {
            consumed.insert(consumed.end(), bytes.begin(), bytes.end());
        });

    expect(
        read_offset == source.size() &&
            consumed.size() == source.size() &&
            std::equal(source.begin(), source.end(), consumed.begin()),
        "one UART_DATA event must drain bytes whose notifications were dropped");
}

void test_ws2815_timing_profiles() {
    using namespace ambilight::firmware;

    constexpr auto classic =
        ws2815_timing(Ws2815TimingProfile::classic);
    constexpr auto revised =
        ws2815_timing(Ws2815TimingProfile::b_v1_revised);

    static_assert(is_valid(classic));
    static_assert(is_valid(revised));
    expect(
        classic == kWs2815ClassicTiming &&
            classic.one_high_ticks == 8 &&
            classic.one_low_ticks == 2,
        "classic profile must preserve the original 1000/250 ns one bit");
    expect(
        revised == kWs2815BV1Timing &&
            revised.one_high_ticks == 6 &&
            revised.one_low_ticks == 5,
        "revised B-V1 profile must use a 750/625 ns one bit");
    expect(
        !is_valid(ws2815_timing(
            static_cast<Ws2815TimingProfile>(255))),
        "an unknown timing profile must not produce a usable waveform");
}

void test_autonomous_effect_mapping() {
    using ambilight::firmware::logical_index_for_physical;

    constexpr std::array<std::size_t, 5> clockwise{2, 3, 4, 0, 1};
    constexpr std::array<std::size_t, 5> counterclockwise{2, 1, 0, 4, 3};
    for (std::size_t physical = 0; physical < clockwise.size(); ++physical) {
        expect(
            logical_index_for_physical(
                physical,
                clockwise.size(),
                2,
                false) == clockwise[physical],
            "clockwise autonomous mapping must match the desktop mapper");
        expect(
            logical_index_for_physical(
                physical,
                counterclockwise.size(),
                2,
                true) == counterclockwise[physical],
            "reversed autonomous mapping must match the desktop mapper");
    }
    expect(
        logical_index_for_physical(7, 0, 3, true) == 0,
        "an empty mapping must remain well-defined");
}

void test_fractional_60_hz_pacing() {
    ambilight::firmware::FractionalTickPacer pacer(1000, 60);
    std::uint32_t total_ticks = 0;
    for (int frame = 0; frame < 60; ++frame) {
        const auto interval = pacer.next_interval();
        expect(
            interval == 16 || interval == 17,
            "60 Hz pacing intervals must be 16 or 17 ticks");
        total_ticks += interval;
    }
    expect(
        total_ticks == 1000,
        "60 frame intervals must total exactly one second");

    pacer.reset_phase();
    expect(
        pacer.next_interval() == 16,
        "resetting a missed deadline must restart the pacing phase");
}

}  // namespace

int main() {
    test_all_color_orders();
    test_rejects_invalid_arguments();
    test_maximum_strip_size();
    test_gpio_allow_list();
    test_device_config_rejects_unknown_color_order();
    test_current_limit_covers_modeled_idle_current();
    test_device_config_structural_equality();
    test_ambient_frame_invalidation_policy();
    test_ambient_lease_accepts_boot_timestamp_zero();
    test_uart_data_notification_drains_all_buffered_bytes();
    test_ws2815_timing_profiles();
    test_autonomous_effect_mapping();
    test_fractional_60_hz_pacing();

    if (failures == 0) {
        std::cout << "All firmware LED wire-format tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
