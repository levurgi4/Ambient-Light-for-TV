cmake_minimum_required(VERSION 3.16)

# Build only the application and its declared ESP-IDF dependencies. This keeps
# unused Wi-Fi/Bluetooth/network components out of the dependency graph.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ambilight_firmware)
