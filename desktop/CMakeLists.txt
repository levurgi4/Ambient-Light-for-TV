cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(AmbilightDesktop VERSION 0.2.9 LANGUAGES CXX)
    include(CTest)
endif()

include(GNUInstallDirs)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

qt_standard_project_setup()

qt_add_executable(ambilight_desktop WIN32
    src/audio/audio_capture_worker.cpp
    src/audio/audio_capture_worker.hpp
    src/audio/audio_spectrum.cpp
    src/audio/audio_spectrum.hpp
    src/audio/audio_types.hpp
    src/app_settings.cpp
    src/app_settings.hpp
    src/autostart.cpp
    src/autostart.hpp
    src/capture/capture_output_selection.hpp
    src/capture/capture_types.hpp
    src/capture/screen_capture_worker.cpp
    src/capture/screen_capture_worker.hpp
    src/led_frame_filter.cpp
    src/led_frame_filter.hpp
    src/led_preview_widget.cpp
    src/led_preview_widget.hpp
    src/main.cpp
    src/main_window.cpp
    src/main_window.hpp
    src/perimeter_mapper.cpp
    src/perimeter_mapper.hpp
    src/transport/serial_transport_worker.cpp
    src/transport/serial_transport_worker.hpp
    src/transport/transport_protocol.cpp
    src/transport/transport_protocol.hpp
    src/transport/transport_types.hpp
)

qt_add_resources(ambilight_desktop ambilight_branding
    PREFIX "/ambilight/branding"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons"
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/app-icon.png"
)

target_compile_features(ambilight_desktop PRIVATE cxx_std_20)
target_link_libraries(ambilight_desktop PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

if(MSVC)
    target_compile_options(ambilight_desktop PRIVATE
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus
    )
else()
    target_compile_options(ambilight_desktop PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

if(TARGET Ambilight::Protocol)
    target_link_libraries(ambilight_desktop PRIVATE Ambilight::Protocol)
endif()

if(WIN32)
    target_sources(ambilight_desktop PRIVATE app.rc)
    target_link_libraries(ambilight_desktop PRIVATE
        d3d11
        d3dcompiler
        dxgi
        ksuser
        ole32
        propsys
        setupapi
        advapi32
        user32
    )
    target_compile_definitions(ambilight_desktop PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

set_target_properties(ambilight_desktop PROPERTIES
    OUTPUT_NAME "Ambilight by Levurgi4"
    WIN32_EXECUTABLE TRUE
)

if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    include(InstallRequiredSystemLibraries)
endif()

install(TARGETS ambilight_desktop
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    BUNDLE DESTINATION .
)

qt_generate_deploy_app_script(
    TARGET ambilight_desktop
    OUTPUT_SCRIPT ambilight_deploy_script
    EXCLUDE_PLUGINS
        qtuiotouchplugin
        qsvgicon
        qsvg
    DEPLOY_TOOL_OPTIONS
        --no-system-d3d-compiler
        --no-opengl-sw
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${ambilight_deploy_script})

if(BUILD_TESTING)
    add_executable(ambilight_audio_spectrum_tests
        src/audio/audio_spectrum.cpp
        src/audio/audio_spectrum.hpp
        tests/audio_spectrum_tests.cpp
    )
    target_compile_features(ambilight_audio_spectrum_tests PRIVATE cxx_std_20)
    target_include_directories(ambilight_audio_spectrum_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ambilight_audio_spectrum_tests PRIVATE Qt6::Gui)

    if(MSVC)
        target_compile_options(ambilight_audio_spectrum_tests PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
        )
    else()
        target_compile_options(ambilight_audio_spectrum_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()

    add_test(NAME desktop.audio_spectrum COMMAND ambilight_audio_spectrum_tests)
    set_tests_properties(desktop.audio_spectrum PROPERTIES
        TIMEOUT 15
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    if(WIN32)
        add_executable(ambilight_audio_device_tests
            src/audio/audio_capture_worker.cpp
            src/audio/audio_capture_worker.hpp
            src/audio/audio_spectrum.cpp
            src/audio/audio_spectrum.hpp
            src/audio/audio_types.hpp
            tests/audio_device_tests.cpp
        )
        target_compile_features(ambilight_audio_device_tests PRIVATE cxx_std_20)
        target_include_directories(ambilight_audio_device_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
        )
        target_link_libraries(ambilight_audio_device_tests PRIVATE
            Qt6::Core
            Qt6::Gui
            ksuser
            ole32
            propsys
        )
        target_compile_definitions(ambilight_audio_device_tests PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
        if(MSVC)
            target_compile_options(ambilight_audio_device_tests PRIVATE
                /W4
                /permissive-
                /utf-8
                /Zc:__cplusplus
            )
        endif()
        add_test(NAME desktop.audio_devices COMMAND ambilight_audio_device_tests)
        set_tests_properties(desktop.audio_devices PROPERTIES
            TIMEOUT 15
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    add_executable(ambilight_desktop_settings_tests
        src/app_settings.cpp
        src/app_settings.hpp
        tests/app_settings_tests.cpp
    )
    target_compile_features(ambilight_desktop_settings_tests PRIVATE cxx_std_20)
    target_include_directories(ambilight_desktop_settings_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ambilight_desktop_settings_tests PRIVATE Qt6::Gui)

    if(MSVC)
        target_compile_options(ambilight_desktop_settings_tests PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
        )
    else()
        target_compile_options(ambilight_desktop_settings_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()

    add_test(NAME desktop.settings COMMAND ambilight_desktop_settings_tests)
    set_tests_properties(desktop.settings PROPERTIES
        TIMEOUT 15
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(ambilight_perimeter_mapper_tests
        src/perimeter_mapper.cpp
        src/perimeter_mapper.hpp
        tests/perimeter_mapper_tests.cpp
    )
    target_compile_features(ambilight_perimeter_mapper_tests PRIVATE cxx_std_20)
    target_include_directories(ambilight_perimeter_mapper_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ambilight_perimeter_mapper_tests PRIVATE Qt6::Gui)

    if(MSVC)
        target_compile_options(ambilight_perimeter_mapper_tests PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
        )
    else()
        target_compile_options(ambilight_perimeter_mapper_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()

    add_test(NAME desktop.perimeter_mapper COMMAND ambilight_perimeter_mapper_tests)
    set_tests_properties(desktop.perimeter_mapper PROPERTIES
        TIMEOUT 15
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(ambilight_led_frame_filter_tests
        src/led_frame_filter.cpp
        src/led_frame_filter.hpp
        tests/led_frame_filter_tests.cpp
    )
    target_compile_features(ambilight_led_frame_filter_tests PRIVATE cxx_std_20)
    target_include_directories(ambilight_led_frame_filter_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ambilight_led_frame_filter_tests PRIVATE Qt6::Gui)

    if(MSVC)
        target_compile_options(ambilight_led_frame_filter_tests PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
        )
    else()
        target_compile_options(ambilight_led_frame_filter_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()

    add_test(NAME desktop.led_frame_filter COMMAND ambilight_led_frame_filter_tests)
    set_tests_properties(desktop.led_frame_filter PROPERTIES
        TIMEOUT 15
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(ambilight_led_preview_widget_tests
        src/led_preview_widget.cpp
        src/led_preview_widget.hpp
        tests/led_preview_widget_tests.cpp
    )
    target_compile_features(ambilight_led_preview_widget_tests PRIVATE cxx_std_20)
    target_include_directories(ambilight_led_preview_widget_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ambilight_led_preview_widget_tests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )

    if(MSVC)
        target_compile_options(ambilight_led_preview_widget_tests PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
        )
    else()
        target_compile_options(ambilight_led_preview_widget_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()

    add_test(
        NAME desktop.led_preview_widget
        COMMAND ambilight_led_preview_widget_tests
    )
    set_tests_properties(desktop.led_preview_widget PROPERTIES
        TIMEOUT 15
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(ambilight_capture_types_tests
        src/capture/capture_output_selection.hpp
        src/capture/capture_types.hpp
        tests/capture_types_tests.cpp
    )
    target_compile_features(ambilight_capture_types_tests PRIVATE cxx_std_20)
    target_include_directories(ambilight_capture_types_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ambilight_capture_types_tests PRIVATE Qt6::Gui)

    if(MSVC)
        target_compile_options(ambilight_capture_types_tests PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
        )
    else()
        target_compile_options(ambilight_capture_types_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()

    add_test(NAME desktop.capture_types COMMAND ambilight_capture_types_tests)
    set_tests_properties(desktop.capture_types PROPERTIES
        TIMEOUT 15
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(ambilight_transport_protocol_tests
        src/transport/transport_protocol.cpp
        src/transport/transport_protocol.hpp
        src/transport/transport_types.hpp
        tests/transport_protocol_tests.cpp
    )
    target_compile_features(ambilight_transport_protocol_tests PRIVATE cxx_std_20)
    target_include_directories(ambilight_transport_protocol_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(ambilight_transport_protocol_tests PRIVATE
        Qt6::Core
        Ambilight::Protocol
    )

    if(MSVC)
        target_compile_options(ambilight_transport_protocol_tests PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
        )
    else()
        target_compile_options(ambilight_transport_protocol_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()

    add_test(
        NAME desktop.transport_protocol
        COMMAND ambilight_transport_protocol_tests
    )
    set_tests_properties(desktop.transport_protocol PROPERTIES
        TIMEOUT 15
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
endif()
