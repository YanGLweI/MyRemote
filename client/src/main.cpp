// Client Agent - Windows Service for Remote Control
// This is the passive endpoint that connects to server

#include <windows.h>
#include <iostream>
#include <memory>
#include <chrono>
#include <fstream>
#include "connection.hpp"
#include "heartbeat.hpp"
#include "auto_reconnect.hpp"
#include "desktop_capture.hpp"
#include "video_encoder.hpp"
#include "input_simulator.hpp"
#include "../common/include/config.hpp"
#include "../common/include/aes_gcm.hpp"
#include "../common/include/ecdh.hpp"

static SERVICE_STATUS g_service_status;
static SERVICE_STATUS_HANDLE g_service_handle;
static std::unique_ptr<Connection> client_connection;
static std::unique_ptr<HeartbeatKeeper> heartbeat_keeper_;
static std::string CONFIG_SERVER_IP = "192.168.1.100";
static int CONFIG_SERVER_PORT = 7500;
static std::string CONFIG_SECRET_KEY = "default_secret_key_12345";

void ServiceMain(int argc, char* argv[]) {
    // Load configuration from file if available
    try {
        auto config = config::load_client_config("config.json");
        CONFIG_SERVER_IP = config.server_ip;
        CONFIG_SERVER_PORT = config.server_port;
        CONFIG_SECRET_KEY = config.secret_key;
        
        std::cout << "Configuration loaded:" << std::endl;
        std::cout << "  Server IP: " << CONFIG_SERVER_IP << std::endl;
        std::cout << "  Server Port: " << CONFIG_SERVER_PORT << std::endl;
        std::cout << "  Device Name: " << config.device_name << std::endl;
    } catch (...) {
        std::cout << "Using default configuration" << std::endl;
    }
    
    // Initialize encryption keys (ECDH key exchange)
    ECDHExchange local_keys;
    try {
        local_keys.generate_keys();
        auto public_key = local_keys.get_public_key();
        std::cout << "Generated ECDH public key (first 32 bytes): ";
        for (int i = 0; i < 32 && i < static_cast<int>(public_key.size()); ++i) {
            printf("%02x", public_key[i]);
        }
        std::cout << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Failed to generate ECDH keys: " << e.what() << std::endl;
        return;
    }
    
    // Create connection manager
    client_connection = std::make_unique<Connection>();
    
    // Set up receive callback for encrypted messages
    client_connection->set_receive_callback([&](const std::vector<uint8_t>& data) {
        try {
            // TODO: Decrypt incoming data using AESGCM
            // For MVP, just log the reception
            std::cout << "Received " << data.size() << " bytes from server" << std::endl;
            
            // Process control commands here
            // Currently placeholder for Protocol Buffers parsing
        } catch (const std::exception& e) {
            std::cerr << "Error handling received message: " << e.what() << std::endl;
        }
    });
    
    // Auto-reconnect handler with exponential backoff
    AutoReconnect reconnect_handler;
    reconnect_handler.set_callback([&]() {
        std::cout << "Attempting reconnection to " << CONFIG_SERVER_IP << ":" 
                  << CONFIG_SERVER_PORT << std::endl;
        
        bool connected = client_connection->connect(CONFIG_SERVER_IP, CONFIG_SERVER_PORT);
        
        if (connected) {
            std::cout << "Connected! Sending registration..." << std::endl;
            
            // TODO: Send ClientHello message with device info
            // Use Protocol Buffers encoding
            
            // Start sending captured frames once authenticated
            capturer.initialize(0);
            
            std::cout << "Ready to send desktop stream" << std::endl;
        }
        
        return connected;
    });
    
    // Initialize desktop capture
    auto capturer = std::make_unique<DesktopCapturer>();
    if (!capturer->initialize(0)) {
        MessageBoxA(nullptr, "Failed to initialize desktop capture", "Error", MB_ICONERROR);
        return;
    }
    
    // Configure capture settings based on quality mode
    EncoderConfig config;
    config.fps = 30;
    config.bitrate_kbps = 2048;
    config.width = GetSystemMetrics(SM_CXSCREEN);
    config.height = GetSystemMetrics(SM_CYSCREEN);
    config.preset = L"RealTime";
    config.quality_level = 70;
    
    capturer->configure(config);
    
    // Initialize video encoder
    auto encoder = std::make_unique<VideoEncoder>();
    if (!encoder->initialize(config)) {
        std::cerr << "Warning: Video encoder initialization failed" << std::endl;
        std::cerr << "Falling back to software encoding or raw capture" << std::endl;
    }
    
    // Input simulator for remote control
    auto input_simulator = std::make_unique<InputSimulator>();
    
    // Heartbeat keeper to maintain connection
    heartbeat_keeper_ = std::make_unique<HeartbeatKeeper>();
    heartbeat_keeper_->start(3000, [&]() {
        // Send heartbeat packet to keep connection alive
        std::vector<uint8_t> heartbeat_data{0x03};  // Message type HEARTBEAT
        bool success = client_connection->send(heartbeat_data);
        
        if (!success) {
            std::cerr << "Heartbeat send failed" << std::endl;
        }
        
        return success;
    });
    
    // Main service loop
    g_service_status.dwCurrentState = SERVICE_RUNNING;
    g_service_status.dwCheckPoint = 0;
    SetServiceStatus(g_service_handle, &g_service_status);
    
    std::cout << "MyRemote Agent service started" << std::endl;
    
    int frame_count = 0;
    time_t last_frame_time = time(nullptr);
    
    // Keep running while accepting connections
    while (g_service_status.dwCurrentState == SERVICE_RUNNING) {
        Sleep(100);
        
        // Check connection status and attempt reconnection if needed
        if (!client_connection->is_connected()) {
            std::cerr << "Disconnected from server! Initiating reconnect..." << std::endl;
            reconnect_handler.on_connection_lost();
            
            // Give reconnection time to establish
            Sleep(3000);
            
            // If now connected, we can start capturing again
            if (client_connection->is_connected()) {
                std::cout << "Connection restored, resuming capture" << std::endl;
            }
        }
        
        // Capture frames at target FPS rate
        CapturedFrame captured_frame;
        if (capturer->capture_frame(captured_frame)) {
            frame_count++;
            
            // Log frame every second
            time_t current_time = time(nullptr);
            if (current_time > last_frame_time + 1) {
                std::cout << "Captured frames so far: " << frame_count 
                          << " (" << frame_count / (current_time - last_frame_time) 
                          << " fps)" << std::endl;
                last_frame_time = current_time;
            }
            
            // Encode to H.264 if encoder is initialized
            if (encoder && encoder->initialized_) {
                if (encoder->encode_frame(
                    captured_frame.raw_bgra.data(),
                    captured_frame.width,
                    captured_frame.height,
                    captured_frame)) {
                    
                    std::cout << "Encoded frame " << captured_frame.frame_number 
                              << ": " << captured_frame.h264_data.size() << " bytes" << std::endl;
                    
                    // TODO: Send encoded frame via connection
                    // client_connection->send(captured_frame.h264_data);
                }
            } else {
                // Fallback: send raw data (for testing/debugging)
                std::cout << "Sending raw frame (no encoder): " 
                          << captured_frame.raw_bgra.size() << " bytes" << std::endl;
                
                // For MVP, just mark this as processed
            }
        } else {
            // Frame too similar to previous one - skip
            Sleep(33);  // Approximate delay for 30fps
        }
    }
    
    std::cout << "Service stopping, cleaning up..." << std::endl;
}

void WINAPI ServiceCtrlHandler(DWORD dwControl) {
    switch (dwControl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            g_service_status.dwWin32ExitCode = NO_ERROR;
            g_service_status.dwCurrentState = SERVICE_STOPPED;
            break;
            
        case SERVICE_CONTROL_PAUSE:
            g_service_status.dwCurrentState = SERVICE_PAUSE_PENDING;
            break;
            
        case SERVICE_CONTROL_INTERROGATE:
            break;
            
        default:
            break;
    }
    
    SetServiceStatus(g_service_handle, &g_service_status);
}

int main(int argc, char* argv[]) {
    // Parse command line arguments for optional overrides
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        
        if (arg == "--ip" && i + 1 < argc) {
            CONFIG_SERVER_IP = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            try {
                CONFIG_SERVER_PORT = std::stoi(argv[++i]);
            } catch (...) {
                std::cerr << "Invalid port number: " << argv[i] << std::endl;
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: agent.exe [--ip SERVER_IP] [--port PORT]\n"
                      << "Server IP defaults to 192.168.1.100\n"
                      << "Port defaults to 7500\n";
            return 0;
        }
    }
    
    // Register service with Windows Service Control Manager
    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwControlsAccepted = 
        SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PAUSE_CONTINUE | SERVICE_ACCEPT_SHUTDOWN;
    g_service_status.dwWin32ExitCode = NO_ERROR;
    g_service_status.dwCheckPoint = 0;
    
    SERVICE_TABLE_ENTRYA service_table[] = {
        {"MyRemoteAgent", (LPSERVICE_CONTROL_HANDLER)ServiceMain},
        {nullptr, nullptr}
    };
    
    if (!StartServiceCtrlDispatcher(service_table)) {
        DWORD error = GetLastError();
        fprintf(stderr, "StartServiceCtrlDispatcher failed (%lu)\n", error);
        
        // For debugging without service host, run standalone
        if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            fprintf(stderr, "\nRunning in standalone mode (not a service)\n");
            fprintf(stderr, "\nArguments:\n");
            fprintf(stderr, "  --ip SERVER_IP  Override server IP address\n");
            fprintf(stderr, "  --port PORT     Override server port\n");
            fprintf(stderr, "  --help          Show this help message\n");
            fprintf(stderr, "\nNote: In standalone mode, press Ctrl+C to exit.\n\n");
            
            // Run directly without service wrapper
            SERVICE_STATUS temp_status{};
            temp_status.dwCurrentState = SERVICE_RUNNING;
            
            // Simulate manual interrupt handling
            std::signal(SIGINT, [](int) {
                temp_status.dwCurrentState = SERVICE_STOPPED;
            });
            
            ServiceMain(argc, argv);
            
            return 0;
        }
        
        return 1;
    }
    
    return 0;
}
