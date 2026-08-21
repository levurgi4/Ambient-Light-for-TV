add_library(ambilight_protocol INTERFACE)
add_library(Ambilight::Protocol ALIAS ambilight_protocol)

target_compile_features(ambilight_protocol INTERFACE cxx_std_20)
target_include_directories(
    ambilight_protocol
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(BUILD_TESTING)
    add_executable(
        ambilight_protocol_tests
        tests/protocol_tests.cpp
    )
    target_link_libraries(ambilight_protocol_tests PRIVATE Ambilight::Protocol)

    if(MSVC)
        target_compile_options(ambilight_protocol_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(ambilight_protocol_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME protocol.unit COMMAND ambilight_protocol_tests)
endif()
