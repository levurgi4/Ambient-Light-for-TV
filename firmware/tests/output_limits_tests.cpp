#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

#include "firmware_types.hpp"
#include "output_limits.hpp"

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_black_frame_has_only_idle_current() {
    using namespace ambilight::firmware;

    std::array<Rgb, kMaximumLedCount> pixels{};
    apply_output_limits(pixels, Rgb{255, 255, 255}, 255, 5000);
    const auto estimate = estimate_output_current(pixels);

    expect(
        estimate.idle_milliamps == 900,
        "300 pixels must include their modeled idle current");
    expect(
        estimate.dynamic_milliamps == 0 &&
            estimate.total_milliamps == 900,
        "a black frame must have no dynamic current");
}

void test_white_frame_is_reduced_to_budget() {
    using namespace ambilight::firmware;

    std::array<Rgb, kMaximumLedCount> pixels{};
    std::fill(pixels.begin(), pixels.end(), Rgb{255, 255, 255});
    apply_output_limits(pixels, Rgb{255, 255, 255}, 255, 5000);
    const auto estimate = estimate_output_current(pixels);

    expect(
        estimate.total_milliamps <= 5000,
        "a full-white maximum strip must fit the configured budget");
    expect(
        pixels.front().red < 255 &&
            pixels.front().red == pixels.front().green &&
            pixels.front().green == pixels.front().blue,
        "current limiting must preserve neutral white");
}

void test_brightness_is_applied_before_current_limit() {
    using namespace ambilight::firmware;

    std::array<Rgb, 1> pixels{Rgb{255, 127, 1}};
    apply_output_limits(pixels, Rgb{255, 255, 255}, 128, 1000);

    expect(
        pixels[0] == Rgb{128, 64, 1},
        "brightness scaling must use rounded 8-bit channels");
}

void test_color_calibration_precedes_global_limits() {
    using namespace ambilight::firmware;

    std::array<Rgb, 1> pixels{Rgb{255, 255, 255}};
    apply_output_limits(pixels, Rgb{255, 128, 64}, 128, 1000);

    expect(
        pixels[0] == Rgb{128, 64, 32},
        "per-channel calibration must precede brightness and current limits");
}

void test_limit_below_idle_forces_black() {
    using namespace ambilight::firmware;

    std::array<Rgb, kMaximumLedCount> pixels{};
    std::fill(pixels.begin(), pixels.end(), Rgb{255, 64, 1});
    apply_output_limits(pixels, Rgb{255, 255, 255}, 255, 500);
    const auto estimate = estimate_output_current(pixels);

    expect(
        std::all_of(
            pixels.begin(),
            pixels.end(),
            [](const Rgb pixel) { return pixel == Rgb{}; }),
        "an impossible below-idle budget must remove all dynamic output");
    expect(
        estimate.dynamic_milliamps == 0 &&
            estimate.total_milliamps == 900,
        "software cannot reduce modeled idle consumption");
}

}  // namespace

int main() {
    test_black_frame_has_only_idle_current();
    test_white_frame_is_reduced_to_budget();
    test_brightness_is_applied_before_current_limit();
    test_color_calibration_precedes_global_limits();
    test_limit_below_idle_forces_black();

    if (failures == 0) {
        std::cout << "All firmware output-limit tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
