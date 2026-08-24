#include "tunnel_manager.hpp"
#include <iostream>
#include <chrono>

TunnelManager::TunnelManager() {
    listener_.set_connected_callback([this](SOCKET socket) {
        std::cout << "New client connected: " << socket << std::endl;
        this->on_client_connected(socket);
    });
    
    listener_.set_disconnected_callback([this](SOCKET socket) {
        std::cout << "Client disconnected: " << socket << std::endl;
        this->on_client_disconnected(socket);
    });
}

TunnelManager::~TunnelManager() {
    stop();
}

bool TunnelManager::start(int port) {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    
    running_.store(true, std::memory_order_release);
    
    // Start listening on specified port
    if (!listener_.start(port)) {
        std::cerr << "Failed to start listener" << std::endl;
        running_.store(false, std::memory_order_release);
        return false;
    }
    
    std::cout << "TunnelManager started, listening on port " << port << std::endl;
    return true;
}

void TunnelManager::stop() {
    if (!running_.exchange(false, std::memory_order_release)) {
        return;
    }
    
    listener_.stop();
    
    // Close all client connections
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        for (auto& [socket, session] : client_pool_) {
            if (session && session->socket != INVALID_SOCKET) {
                closesocket(session->socket);
                session->active = false;
            }
        }
        client_pool_.clear();
        device_id_to_socket_.clear();
    }
    
    client_count_ = 0;
    std::cout << "TunnelManager stopped" << std::endl;
}

bool TunnelManager::is_running() const {
    return running_.load(std::memory_order_acquire);
}

SOCKET TunnelManager::register_client(SOCKET socket, const std::string& device_id) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    auto session = std::make_shared<ClientSession>();
    session->socket = socket;
    session->device_id = device_id;
    session->device_name = device_id.empty() ? "Unknown Device" : device_id.substr(7);  // Remove prefix
    session->screen_width = GetSystemMetrics(SM_CXSCREEN);
    session->screen_height = GetSystemMetrics(SM_CYSCREEN);
    session->active = true;
    session->connect_time = std::time(nullptr);
    
    client_pool_[socket] = session;
    if (!device_id.empty()) {
        device_id_to_socket_[device_id] = socket;
    }
    
    int count = ++client_count_;
    
    std::cout << "Registered client: " << device_id << ", total: " << count << std::endl;
    
    return socket;
}

void TunnelManager::unregister_client(SOCKET socket) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    auto it = client_pool_.find(socket);
    if (it == client_pool_.end()) {
        return;
    }
    
    auto session = it->second;
    std::string device_id = session->device_id;
    
    // Mark as inactive before closing socket
    session->active = false;
    
    if (socket != INVALID_SOCKET) {
        closesocket(socket);
    }
    
    client_pool_.erase(it);
    
    auto device_it = device_id_to_socket_.find(device_id);
    if (device_it != device_id_to_socket_.end()) {
        device_id_to_socket_.erase(device_it);
    }
    
    int count = --client_count_;
    std::cout << "Unregistered client: " << device_id << ", remaining: " << count << std::endl;
}

std::shared_ptr<ClientSession> TunnelManager::get_client_by_device_id(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    auto it = device_id_to_socket_.find(device_id);
    if (it != device_id_to_socket_.end()) {
        SOCKET sock = it->second;
        auto session_it = client_pool_.find(sock);
        if (session_it != client_pool_.end()) {
            return session_it->second;
        }
    }
    
    return nullptr;
}

std::vector<std::shared_ptr<ClientSession>> TunnelManager::get_all_clients() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    std::vector<std::shared_ptr<ClientSession>> result;
    
    for (const auto& [sock, session] : client_pool_) {
        if (session && session->active) {
            result.push_back(session);
        }
    }
    
    return result;
}

bool TunnelManager::send_to_client(SOCKET socket, const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    auto it = client_pool_.find(socket);
    if (it == client_pool_.end() || !it->second->active) {
        return false;
    }
    
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        int sent = send(socket, 
                       reinterpret_cast<const char*>(data.data() + total_sent),
                       static_cast<int>(data.size() - total_sent),
                       0);
        
        if (sent == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAETIMEDOUT) {
                Sleep(10);
                continue;
            }
            
            std::cerr << "Send failed to socket " << socket << ": " << error << std::endl;
            return false;
        }
        
        if (sent == 0) {
            return false;
        }
        
        total_sent += sent;
    }
    
    return true;
}
