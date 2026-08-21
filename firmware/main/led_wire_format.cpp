#include "led_wire_format.hpp"

#include <array>
#include <limits>

namespace ambilight::firmware {
namespace {

[[nodiscard]] bool ordered_channels(
    const Rgb pixel,
    const ColorOrder order,
    std::array<std::uint8_t, kLedBytesPerPixel>& destination) noexcept {
    switch (order) {
        case ColorOrder::rgb:
            destination = {pixel.red, pixel.green, pixel.blue};
            return true;
        case ColorOrder::rbg:
            destination = {pixel.red, pixel.blue, pixel.green};
            return true;
        case ColorOrder::grb:
            destination = {pixel.green, pixel.red, pixel.blue};
            return true;
        case ColorOrder::gbr:
            destination = {pixel.green, pixel.blue, pixel.red};
            return true;
        case ColorOrder::brg:
            destination = {pixel.blue, pixel.red, pixel.green};
            return true;
        case ColorOrder::bgr:
            destination = {pixel.blue, pixel.green, pixel.red};
            return true;
    }
    return false;
}

}  // namespace

bool encode_led_wire_bytes(
    const std::span<const Rgb> pixels,
    const ColorOrder order,
    const std::span<std::uint8_t> destination) noexcept {
    if (pixels.size() >
            std::numeric_limits<std::size_t>::max() / kLedBytesPerPixel ||
        destination.size() != pixels.size() * kLedBytesPerPixel) {
        return false;
    }

    std::array<std::uint8_t, kLedBytesPerPixel> channels{};
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        if (!ordered_channels(pixels[index], order, channels)) {
            return false;
        }

        const auto byte_offset = index * kLedBytesPerPixel;
        destination[byte_offset] = channels[0];
        destination[byte_offset + 1U] = channels[1];
        destination[byte_offset + 2U] = channels[2];
    }
    return true;
}

}  // namespace ambilight::firmware
