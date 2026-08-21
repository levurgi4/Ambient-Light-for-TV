cmake_minimum_required(VERSION 3.20)

project(ambilight_firmware_host_tests LANGUAGES CXX)

enable_testing()

add_executable(firmware.led_wire_format
    led_wire_format_tests.cpp
    ../main/led_wire_format.cpp
)
target_include_directories(firmware.led_wire_format PRIVATE ../main)
target_compile_features(firmware.led_wire_format PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(firmware.led_wire_format PRIVATE /W4 /permissive-)
else()
    target_compile_options(firmware.led_wire_format PRIVATE
        -Wall
        -Wextra
        -Wconversion
        -Wshadow
    )
endif()

add_test(NAME firmware.led_wire_format COMMAND firmware.led_wire_format)

add_executable(firmware.output_limits
    output_limits_tests.cpp
    ../main/output_limits.cpp
)
target_include_directories(firmware.output_limits PRIVATE ../main)
target_compile_features(firmware.output_limits PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(firmware.output_limits PRIVATE /W4 /permissive-)
else()
    target_compile_options(firmware.output_limits PRIVATE
        -Wall
        -Wextra
        -Wconversion
        -Wshadow
    )
endif()

add_test(NAME firmware.output_limits COMMAND firmware.output_limits)

add_executable(firmware.ambient_frame_lifecycle
    ambient_frame_lifecycle_tests.cpp
)
target_include_directories(
    firmware.ambient_frame_lifecycle
    PRIVATE
        ../main
)
target_compile_features(
    firmware.ambient_frame_lifecycle
    PRIVATE
        cxx_std_20
)

if(MSVC)
    target_compile_options(
        firmware.ambient_frame_lifecycle
        PRIVATE
            /W4
            /permissive-
    )
else()
    target_compile_options(
        firmware.ambient_frame_lifecycle
        PRIVATE
            -Wall
            -Wextra
            -Wconversion
            -Wshadow
    )
endif()

add_test(
    NAME firmware.ambient_frame_lifecycle
    COMMAND firmware.ambient_frame_lifecycle
)
