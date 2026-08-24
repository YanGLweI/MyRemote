#pragma once

#include <unordered_map>
#include <string>
#include <memory>
#include <mutex>
#include <winsock2.h>
#include "listener.hpp"

// Client session data structure
struct ClientSession {
    SOCKET socket;
    std::string device_id;
    std::string device_name;
    std::string secret_key;
    int screen_width;
    int screen_height;
    bool active;
    time_t connect_time;
};

// Tunnel manager - manages all client connections and communication tunnels
class TunnelManager {
public:
    TunnelManager();
    ~TunnelManager();
    
    // Start managing tunnel connections
    bool start(int port);
    
    // Stop and cleanup all tunnels
    void stop();
    
    // Check if server is active
    bool is_running() const;
    
    // Register a new client connection (call when accept loop receives connection)
    SOCKET register_client(SOCKET socket, const std::string& device_id);
    
    // Unregister client when disconnected
    void unregister_client(SOCKET socket);
    
    // Get client by device ID
    std::shared_ptr<ClientSession> get_client_by_device_id(const std::string& device_id);
    
    // Get all active clients for UI display
    std::vector<std::shared_ptr<ClientSession>> get_all_clients();
    
    // Broadcast message to all clients
    bool broadcast_to_clients(const std::vector<uint8_t>& data);
    
    // Send specific message to single client
    bool send_to_client(SOCKET socket, const std::vector<uint8_t>& data);
    
private:
    Listener listener_;
    std::unordered_map<SOCKET, std::shared_ptr<ClientSession>> client_pool_;
    std::unordered_map<std::string, SOCKET> device_id_to_socket_;
    mutable std::mutex pool_mutex_;
    
    std::atomic<bool> running_{false};
    std::atomic<int> client_count_{0};
    
    // Callback to handle incoming messages from clients
    using MessageCallback = std::function<void(SOCKET socket, const std::vector<uint8_t>& data)>;
    MessageCallback message_callback_;
    
    void handle_incoming_message(SOCKET socket, const std::vector<uint8_t>& data);
    
    void on_client_connected(SOCKET socket);
    void on_client_disconnected(SOCKET socket);
};
