idf_component_register(
    SRCS
        "app_main.cpp"
        "device_state.cpp"
        "effects_engine.cpp"
        "led_output.cpp"
        "led_wire_format.cpp"
        "output_limits.cpp"
        "protocol_dispatcher.cpp"
        "uart_transport.cpp"
    INCLUDE_DIRS
        "."
        "../../shared/protocol/include"
    REQUIRES
        esp_driver_gpio
        esp_driver_rmt
        esp_driver_uart
        esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Wconversion
    -Wshadow
)
