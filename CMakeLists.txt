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
find_package(Protobuf REQUIRED)
include_directories(${PROTOBUF_INCLUDE_DIR})

# Generate protocol buffer sources
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS
    common/include/protocol.proto
)

# Windows 特定库 (桌面捕获和编码)
if(WIN32)
    find_package(DXGUID REQUIRED)
    # Media Foundation - 动态链接避免 DLL 依赖问题
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
    ${PROTOBUF_LIBRARIES}
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
        client/src/client_agent.cpp
    )
    target_link_libraries(client_lib PRIVATE
        common_lib
        Boost::system Boost::thread Boost::chrono
        ws2_32 Imm32 User32
        dxguid d3d11
    )
    
    # Client 可执行文件
    add_executable(agent WIN32 client/src/main.cpp)
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
            Boost::system Boost::thread
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
install(TARGETS agent control_server
    RUNTIME DESTINATION bin
)
