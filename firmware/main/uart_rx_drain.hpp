#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ambilight::firmware {

// Drain all bytes currently available from the UART ring buffer after any one
// UART_DATA notification. ESP-IDF can retain RX bytes even when its event queue
// is full and the matching notification is dropped, so trusting event.size
// alone can strand an otherwise complete packet indefinitely.
template <typename Reader, typename Consumer>
void drain_available_uart_bytes(
    const std::span<std::uint8_t> scratch,
    Reader&& reader,
    Consumer&& consumer) {
    if (scratch.empty()) {
        return;
    }

    for (;;) {
        const int received = reader(scratch);
        if (received <= 0) {
            return;
        }

        const auto received_size = static_cast<std::size_t>(received);
        if (received_size > scratch.size()) {
            return;
        }
        consumer(std::span<const std::uint8_t>(
            scratch.data(),
            received_size));
    }
}

}  // namespace ambilight::firmware
