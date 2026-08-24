#pragma once

#include <string>
#include <unordered_map>
#include <functional>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

// Server-side listener module for accepting client connections
class Listener {
public:
    Listener();
    ~Listener();
    
    // Start listening on specified port (blocking)
    bool start(int port);
    
    // Stop listening and close all connections
    void stop();
    
    // Check if server is running
    bool is_running() const { return running_.load(std::memory_order_acquire); }
    
    // Callback type for client connection events
    using ClientConnectedCallback = std::function<void(SOCKET socket)>;
    using ClientDisconnectedCallback = std::function<void(SOCKET socket)>;
    
    // Set callbacks
    void set_connected_callback(ClientConnectedCallback callback);
    void set_disconnected_callback(ClientDisconnectedCallback callback);
    
private:
    SOCKET listen_socket_;
    std::atomic<bool> running_{false};
    std::atomic<bool> should_stop_{false};
    
    ClientConnectedCallback connected_callback_;
    ClientDisconnectedCallback disconnected_callback_;
    
    std::thread accept_thread_;
    
    // Initialize WinSock
    static bool init_winsock();
    
    // Main accept loop
    void accept_loop();
    
    // Handle a new client connection
    void handle_client(SOCKET client_socket);
};
