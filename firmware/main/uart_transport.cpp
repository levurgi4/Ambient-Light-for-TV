#include "uart_transport.hpp"

#include <array>
#include <cstddef>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_num.h"

#include "uart_rx_drain.hpp"

namespace ambilight::firmware {
namespace {

constexpr int kReceiveBufferBytes = 8192;
constexpr int kTransmitBufferBytes = 4096;
constexpr int kEventQueueDepth = 20;
constexpr std::size_t kReadChunkBytes = 1024;

[[nodiscard]] bool is_receive_error(const uart_event_type_t type) {
    return type == UART_FIFO_OVF ||
           type == UART_BUFFER_FULL ||
           type == UART_BREAK ||
           type == UART_PARITY_ERR ||
           type == UART_FRAME_ERR;
}

}  // namespace

UartTransport::~UartTransport() {
    if (driver_installed_) {
        uart_driver_delete(kPort);
    }
}

void UartTransport::attach(ByteStreamSink& sink) {
    sink_ = &sink;
}

bool UartTransport::start() {
    if (sink_ == nullptr || driver_installed_) {
        return false;
    }

    const uart_config_t uart_config{
        .baud_rate = kBaudRate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {},
    };

    if (uart_param_config(kPort, &uart_config) != ESP_OK ||
        uart_set_pin(
            kPort,
            GPIO_NUM_1,
            GPIO_NUM_3,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE) != ESP_OK ||
        uart_driver_install(
            kPort,
            kReceiveBufferBytes,
            kTransmitBufferBytes,
            kEventQueueDepth,
            &event_queue_,
            0) != ESP_OK) {
        event_queue_ = nullptr;
        return false;
    }

    driver_installed_ = true;
    uart_flush_input(kPort);

    // Terminate any 115200-baud ROM boot garbage in the host decoder before
    // the first valid 2 Mbaud COBS response is sent.
    constexpr std::uint8_t stream_delimiter = 0;
    if (uart_write_bytes(kPort, &stream_delimiter, 1) != 1) {
        uart_driver_delete(kPort);
        event_queue_ = nullptr;
        driver_installed_ = false;
        return false;
    }

    if (xTaskCreatePinnedToCore(
            task_entry,
            "uart_rx",
            6144,
            this,
            12,
            nullptr,
            0) != pdPASS) {
        uart_driver_delete(kPort);
        event_queue_ = nullptr;
        driver_installed_ = false;
        return false;
    }

    return true;
}

bool UartTransport::write(const std::span<const std::uint8_t> bytes) {
    if (!driver_installed_ || bytes.empty()) {
        return false;
    }

    const auto written = uart_write_bytes(
        kPort,
        bytes.data(),
        static_cast<std::size_t>(bytes.size()));
    return written == static_cast<int>(bytes.size());
}

void UartTransport::task_entry(void* context) {
    static_cast<UartTransport*>(context)->run();
}

void UartTransport::run() {
    std::array<std::uint8_t, kReadChunkBytes> buffer{};
    uart_event_t event{};

    for (;;) {
        if (xQueueReceive(event_queue_, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (event.type == UART_DATA) {
            drain_available_uart_bytes(
                buffer,
                [](const std::span<std::uint8_t> destination) {
                    return uart_read_bytes(
                        kPort,
                        destination.data(),
                        static_cast<std::uint32_t>(destination.size()),
                        0);
                },
                [this](const std::span<const std::uint8_t> received) {
                    sink_->consume(received);
                });
        } else if (is_receive_error(event.type)) {
            // Drop corrupted/overflowed bytes and inject a COBS delimiter so a
            // partial packet already held by the stream decoder cannot merge
            // with the next valid packet. Pause the RX producer while both its
            // ring buffer and event queue are reset. Resetting the queue after
            // an unguarded flush could otherwise discard the only UART_DATA
            // notification for bytes that arrived between the two calls.
            static_cast<void>(uart_disable_rx_intr(kPort));
            xQueueReset(event_queue_);
            static_cast<void>(uart_flush_input(kPort));
            static_cast<void>(uart_enable_rx_intr(kPort));
            constexpr std::array<std::uint8_t, 1> delimiter{0};
            sink_->consume(delimiter);
        }
    }
}

}  // namespace ambilight::firmware
