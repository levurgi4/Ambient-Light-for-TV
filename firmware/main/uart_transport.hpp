#pragma once

#include <cstdint>
#include <span>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "byte_stream.hpp"

namespace ambilight::firmware {

class UartTransport final : public ByteStreamWriter {
public:
    static constexpr uart_port_t kPort = UART_NUM_0;
    static constexpr int kBaudRate = 2'000'000;

    UartTransport() = default;
    ~UartTransport() override;

    UartTransport(const UartTransport&) = delete;
    UartTransport& operator=(const UartTransport&) = delete;

    void attach(ByteStreamSink& sink);
    [[nodiscard]] bool start();
    [[nodiscard]] bool write(
        std::span<const std::uint8_t> bytes) override;

private:
    static void task_entry(void* context);
    void run();

    ByteStreamSink* sink_{};
    QueueHandle_t event_queue_{};
    bool driver_installed_{};
};

}  // namespace ambilight::firmware
