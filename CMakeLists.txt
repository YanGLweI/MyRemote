cmake_minimum_required(VERSION 3.16)
project(MyRemoteControl VERSION 1.0.0 LANGUAGES CXX)
enable_language(RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 静态 CRT，配合 OpenSSL 静态链接，客户端可产出单文件
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(BUILD_SERVER "Build server control端" ON)
option(BUILD_CLIENT "Build client agent" ON)

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_compile_options(/utf-8)

# Qt6 仅服务端需要
if(BUILD_SERVER)
    find_package(Qt6 COMPONENTS Widgets REQUIRED)
    set(CMAKE_AUTOMOC ON)
endif()

# OpenSSL（优先静态链接）
set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED)

# OpenH264 (via vcpkg static triplet)
set(VCPKG_STATIC_ROOT "C:/vcpkg/installed/x64-windows-static"
    CACHE PATH "vcpkg x64-windows-static installed tree")
add_library(openh264 STATIC IMPORTED)
set_target_properties(openh264 PROPERTIES
    IMPORTED_LOCATION "${VCPKG_STATIC_ROOT}/lib/openh264.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${VCPKG_STATIC_ROOT}/include")

include_directories(
    ${CMAKE_SOURCE_DIR}/common/include
    ${CMAKE_SOURCE_DIR}/client/include
    ${CMAKE_SOURCE_DIR}/server/include
)

# 公共库：协议帧编解码、加密、配置
add_library(common_lib STATIC
    common/src/frame_codec.cpp
    common/src/messages.cpp
    common/src/log.cpp
    common/src/crypto.cpp
    common/src/config.cpp
)
target_link_libraries(common_lib PUBLIC
    OpenSSL::Crypto
    ws2_32
)

set(WINDOWS_SYS_LIBS ws2_32 user32 gdi32 advapi32 wtsapi32)

# 客户端（被控端）
if(BUILD_CLIENT)
    add_executable(agent WIN32
        client/src/main.cpp
        client/src/desktop.cpp
        client/src/service.cpp
        client/src/config_gui.cpp
        client/src/tray_icon.cpp
        client/resources/agent.rc
        client/src/connection.cpp
        client/src/desktop_capture.cpp
        client/src/video_encoder.cpp
        client/src/input_simulator.cpp
        client/src/heartbeat.cpp
        client/src/device_id.cpp
    )
    target_link_libraries(agent PRIVATE
        common_lib
        ${WINDOWS_SYS_LIBS}
        iphlpapi shell32
        d3d11 dxgi dwmapi
        openh264
    )
    set_target_properties(agent PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

# 服务端（控制端）
if(BUILD_SERVER)
    add_executable(control_server WIN32
        server/src/main.cpp
        server/src/main_window.cpp
        server/src/sessions_area.cpp
        server/src/session_view.cpp
        server/src/session_toolbar.cpp
        server/src/elided_label.cpp
        server/src/listener.cpp
        server/src/tunnel_manager.cpp
        server/src/display_renderer.cpp
        server/src/input_gateway.cpp
        server/src/device_panel.cpp
        server/src/device_list_model.cpp
        server/src/device_row_delegate.cpp
        server/src/remote_controller.cpp
        server/src/frame_pipeline.cpp
        server/src/h264_decoder.cpp
        # Q_OBJECT headers must be listed so AUTOMOC generates their moc files
        server/include/tunnel_manager.hpp
        server/include/main_window.hpp
        server/include/sessions_area.hpp
        server/include/session_view.hpp
        server/include/session_toolbar.hpp
        server/include/elided_label.hpp
        server/include/device_panel.hpp
        server/include/device_list_model.hpp
        server/include/device_row_delegate.hpp
        server/include/display_renderer.hpp
        server/include/input_gateway.hpp
        server/include/remote_controller.hpp
        server/include/frame_pipeline.hpp
    )
    target_link_libraries(control_server PRIVATE
        common_lib
        Qt6::Widgets
        ${WINDOWS_SYS_LIBS}
        d3d11 dxgi
        openh264
    )
    set_target_properties(control_server PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

set(INSTALL_TARGETS "")
if(BUILD_CLIENT)
    list(APPEND INSTALL_TARGETS agent)
endif()
if(BUILD_SERVER)
    list(APPEND INSTALL_TARGETS control_server)
endif()
if(INSTALL_TARGETS)
    install(TARGETS ${INSTALL_TARGETS} RUNTIME DESTINATION bin)
endif()
