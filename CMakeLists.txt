cmake_minimum_required(VERSION 3.24)

project(
    AmbilightByLevurgi4
    VERSION 0.2.11
    DESCRIPTION "Low-latency Ambilight controller for Windows and ESP32"
    LANGUAGES CXX
)

include(CTest)

option(AMBILIGHT_BUILD_DESKTOP "Build the Windows desktop application" ON)

add_subdirectory(shared/protocol)

if(AMBILIGHT_BUILD_DESKTOP)
    if(NOT WIN32)
        message(FATAL_ERROR "The desktop application currently supports Windows only.")
    endif()
    add_subdirectory(desktop)
endif()

install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    DESTINATION .
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES" DESTINATION .)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Colour test" DESTINATION .)
