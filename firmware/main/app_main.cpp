#include "device_state.hpp"
#include "effects_engine.hpp"
#include "led_output.hpp"
#include "led_output_config.hpp"
#include "protocol_dispatcher.hpp"
#include "uart_transport.hpp"

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace ambilight::firmware {
namespace {

[[nodiscard]] constexpr Ws2815TimingProfile configured_timing_profile() {
#if CONFIG_AMBILIGHT_WS2815_TIMING_B_V1_REVISED
    return Ws2815TimingProfile::b_v1_revised;
#else
    return Ws2815TimingProfile::classic;
#endif
}

[[noreturn]] void idle_safely() {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace
}  // namespace ambilight::firmware

extern "C" void app_main() {
    using namespace ambilight::firmware;

    static DeviceState state;
#if CONFIG_AMBILIGHT_LED_DATA_GPIO < 0
    static NoopLedOutput led_output;
#else
    static_assert(
        is_safe_led_data_gpio(CONFIG_AMBILIGHT_LED_DATA_GPIO),
        "CONFIG_AMBILIGHT_LED_DATA_GPIO is not safe for ESP32-WROOM-32D");
    static RmtLedOutput led_output(
        CONFIG_AMBILIGHT_LED_DATA_GPIO,
        configured_timing_profile());
#endif
    const auto output_initialization =
        led_output.initialize(state.config());
    state.set_output_status(
        led_output.has_physical_output(),
        output_initialization == ESP_OK && led_output.is_ready());

    // Rendering starts in OFF mode. With the default GPIO=-1 build the selected
    // backend is a strict no-op and no LED GPIO is configured. A failed
    // physical initialization remains observable over GET_STATE; the render
    // task retries a complete RMT reinitialization at a bounded rate.
    static EffectsEngine effects(state, led_output);
    if (!effects.start()) {
        led_output.shutdown();
        idle_safely();
    }

    static UartTransport transport;
    static ProtocolDispatcher protocol(state, transport);
    transport.attach(protocol);
    if (!transport.start()) {
        static_cast<void>(
            state.set_mode(OperatingMode::off, EffectSettings{}));
        idle_safely();
    }

    vTaskDelete(nullptr);
}
