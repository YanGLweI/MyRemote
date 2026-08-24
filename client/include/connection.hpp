#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <thread>
#include <atomic>
#include <functional>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

// Client connection manager - active outbound connection only
// This is the ONLY network path from client to server
class Connection {
public:
    Connection();
    ~Connection();
    
    // Connect to server at specified IP and port
    bool connect(const std::string& server_ip, int port);
    
    // Disconnect immediately
    void disconnect();
    
    // Check if connected
    bool is_connected() const { return connected_.load(std::memory_order_acquire); }
    
    // Send encrypted data buffer (blocks until complete)
    bool send(const std::vector<uint8_t>& data);
    
    // Set callback for received data
    using ReceiveCallback = std::function<void(const std::vector<uint8_t>&)>;
    void set_receive_callback(ReceiveCallback callback);
    
private:
    SOCKET socket_;
    std::thread recv_thread_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> should_stop_{false};
    
    ReceiveCallback receive_callback_;
    
    // Initialize winsock
    static bool init_winsock();
    
    // Receive thread entry point
    void receive_loop();
};
