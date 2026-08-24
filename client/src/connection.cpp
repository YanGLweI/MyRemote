#include "connection.hpp"
#include <iostream>
#include <functional>

Connection::Connection() : socket_(INVALID_SOCKET) {
    init_winsock();
}

Connection::~Connection() {
    disconnect();
    
    if (wsa_data_initialized) {
        WSACleanup();
    }
}

bool Connection::init_winsock() {
    if (wsa_data_initialized) return true;
    
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cerr << "WSAStartup failed: " << GetLastError() << std::endl;
        return false;
    }
    
    wsa_data_initialized = TRUE;
    return true;
}

bool Connection::connect(const std::string& server_ip, int port) {
    if (connected_.load(std::memory_order_acquire)) {
        return true;
    }
    
    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET) {
        std::cerr << "Socket creation failed: " << WSAGetLastError() << std::endl;
        return false;
    }
    
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);
    
    if (connect(socket_, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) != 0 && 
        WSAGetLastError() != WSAEWOULDBLOCK) {
        std::cerr << "Connect failed: " << WSAGetLastError() << std::endl;
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }
    
    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(socket_, &writefds);
    
    timeval timeout{10, 0};
    if (select(0, nullptr, &writefds, nullptr, &timeout) <= 0) {
        std::cerr << "Connection timeout" << std::endl;
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }
    
    int error = 0;
    int error_len = sizeof(error);
    getsockopt(socket_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &error_len);
    
    if (error != 0) {
        std::cerr << "Socket error after connect: " << error << std::endl;
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }
    
    connected_.store(true, std::memory_order_release);
    
    should_stop_.store(false, std::memory_order_release);
    recv_thread_ = std::thread(&Connection::receive_loop, this);
    
    std::cout << "Connected to " << server_ip << ":" << port << std::endl;
    return true;
}

void Connection::disconnect() {
    if (!connected_.exchange(false, std::memory_order_release)) {
        return;
    }
    
    should_stop_.store(true, std::memory_order_release);
    
    if (socket_ != INVALID_SOCKET) {
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    
    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }
    
    std::cout << "Disconnected" << std::endl;
}

bool Connection::send(const std::vector<uint8_t>& data) {
    if (!is_connected() || socket_ == INVALID_SOCKET) {
        return false;
    }
    
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        int sent = send(socket_, 
                       reinterpret_cast<const char*>(data.data() + total_sent),
                       static_cast<int>(data.size() - total_sent),
                       0);
        
        if (sent == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                Sleep(10);
                continue;
            }
            
            std::cerr << "Send failed: " << error << std::endl;
            disconnect();
            return false;
        }
        
        if (sent == 0) {
            disconnect();
            return false;
        }
        
        total_sent += sent;
    }
    
    return true;
}

void Connection::set_receive_callback(ReceiveCallback callback) {
    receive_callback_ = std::move(callback);
}

void Connection::receive_loop() {
    std::vector<uint8_t> read_buffer(65536);
    
    while (!should_stop_.load(std::memory_order_acquire)) {
        int received = recv(socket_, 
                           reinterpret_cast<char*>(read_buffer.data()),
                           static_cast<int>(read_buffer.size()),
                           0);
        
        if (received <= 0) {
            if (received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                Sleep(10);
                continue;
            }
            
            break;
        }
        
        if (receive_callback_) {
            receive_callback_(read_buffer.subspan(0, received));
        }
    }
    
    disconnect();
}
