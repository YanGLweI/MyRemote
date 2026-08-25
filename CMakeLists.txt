cmake_minimum_required(VERSION 3.16)
project(MyRemoteControl VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 全局编译选项
option(BUILD_SERVER "Build server control端" ON)
option(BUILD_CLIENT "Build client agent" ON)

# Boost is optional for client
option(BOOST_REQUIRED "Require Boost" OFF)
if(BUILD_CLIENT OR BUILD_SERVER)
    if(BOOST_REQUIRED)
        find_package(Boost REQUIRED COMPONENTS system thread chrono)
    else()
        find_package(Boost COMPONENTS system thread chrono)
        if(Boost_FOUND)
            message(STATUS "Boost found, using it")
        else()
            message(WARNING "Boost not found, some features may be disabled")
        endif()
    endif()
endif()
# Qt6 is optional - only needed for server
# OpenSSL is required for encryption (used by common_lib)
if(WIN32)
    # On Windows, try to find OpenSSL
    message(STATUS "Looking for OpenSSL...")
    if(DEFINED ENV{OPENSSL_ROOT_DIR})
        message(STATUS "OPENSSL_ROOT_DIR from env: $ENV{OPENSSL_ROOT_DIR}")
    endif()
    find_package(OpenSSL)
    if(OpenSSL_FOUND)
        message(STATUS "✅ OpenSSL found")
    else()
        message(FATAL_ERROR 
            "❌ OpenSSL not found!\n"
            "Please ensure OpenSSL is installed via Chocolatey:\n"
            "  choco install openssl -y\n"
            "Or set OPENSSL_ROOT_DIR environment variable."
        )
    endif()
endif()
set(BUILD_SERVER_WITH_QT OFF)
if(BUILD_SERVER)
    find_package(Qt6 COMPONENTS Widgets Concurrent Network QUIET)
    if(Qt6_FOUND)
        set(BUILD_SERVER_WITH_QT ON)
        message(STATUS "Qt6 found - building server with Qt GUI")
    else()
        message(WARNING "Qt6 not found - skipping server build (requires Qt)")
    endif()
endif()

# Protocol Buffers
find_package(Protobuf)
if(Protobuf_FOUND)
    include_directories(${PROTOBUF_INCLUDE_DIR})
    message(STATUS "Protobuf found")
else()
    # Use alternative or skip protobuf generation
    message(WARNING "Protobuf not found! Some features may be disabled.")
    set(PROTO_SRCS "")
    set(PROTO_HDRS "")
endif()

# Generate protocol buffer sources
if(Protobuf_FOUND)
    protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS
        common/include/protocol.proto
    )
else()
    message(WARNING "Skipping proto generation - no protoc found")
endif()

# Windows 特定库 (桌面捕获和编码)
if(WIN32)
    # No additional Windows-specific libraries needed (already in client_lib)
endif()

# 头文件目录
include_directories(${CMAKE_SOURCE_DIR}/common/include)
include_directories(${CMAKE_SOURCE_DIR}/client/include)
include_directories(${CMAKE_SOURCE_DIR}/server/include)

# Common library (protocol, encryption, etc.)
add_library(common_lib STATIC
    ${PROTO_SRCS}
    common/src/aes_gcm.cpp
    common/src/ecdh.cpp
)
target_link_libraries(common_lib PUBLIC
    $<$<BOOL:${Protobuf_FOUND}>:${PROTOBUF_LIBRARIES}>
    OpenSSL::Crypto
)

# 客户端库
if(BUILD_CLIENT)
    add_library(client_lib STATIC
        client/src/connection.cpp
        client/src/desktop_capture.cpp
        client/src/video_encoder.cpp
        client/src/input_simulator.cpp
        client/src/heartbeat.cpp
        client/src/auto_reconnect.cpp
    )
    target_link_libraries(client_lib PRIVATE
        common_lib
        $<$<BOOL:${Boost_FOUND}>:Boost::system Boost::thread Boost::chrono>
        ws2_32 Imm32 User32
        dxguid d3d11
    )
    
    # Client 可执行文件
    add_executable(agent client/src/main.cpp)
    target_link_libraries(agent PRIVATE client_lib)
    set_target_properties(agent PROPERTIES
        OUTPUT_NAME "agent"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/"
    )
endif()

# 服务端库
if(BUILD_SERVER)
    if(BUILD_SERVER_WITH_QT)
        add_library(server_lib STATIC
            server/src/listener.cpp
            server/src/tunnel_manager.cpp
            server/src/display_renderer.cpp
            server/src/device_list.cpp
            server/src/remote_controller.cpp
        )
        target_link_libraries(server_lib PRIVATE
            common_lib
            $<$<BOOL:${Boost_FOUND}>:Boost::system Boost::thread>
            Qt6::Widgets Qt6::Concurrent Qt6::Network
            ws2_32
        )
        
        # Server 可执行文件
        add_executable(control_server WIN32 server/src/main.cpp)
        target_link_libraries(control_server PRIVATE server_lib)
        set_target_properties(control_server PROPERTIES
            OUTPUT_NAME "control_server"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/"
        )
    else()
        message(FATAL_ERROR "Server build requested but Qt6 not found. Install Qt6 or set BUILD_SERVER=OFF")
    endif()
endif()

# Install rules
install(TARGETS agent
    RUNTIME DESTINATION bin
)
if(BUILD_SERVER AND BUILD_SERVER_WITH_QT)
    install(TARGETS control_server
        RUNTIME DESTINATION bin
    )
endif()
