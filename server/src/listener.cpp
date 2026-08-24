#include "listener.hpp"
#include <iostream>
#include <chrono>

static BOOL wsa_data_initialized = FALSE;
static WSADATA wsa_data;

Listener::Listener() : listen_socket_(INVALID_SOCKET) {}

Listener::~Listener() {
    stop();
    
    if (wsa_data_initialized) {
        WSACleanup();
    }
}

bool Listener::init_winsock() {
    if (wsa_data_initialized) return true;
    
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        std::cerr << "WSAStartup failed: " << GetLastError() << std::endl;
        return false;
    }
    
    wsa_data_initialized = TRUE;
    return true;
}

bool Listener::start(int port) {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    
    if (!init_winsock()) {
        return false;
    }
    
    // Create listening socket
    listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket_ == INVALID_SOCKET) {
        std::cerr << "Failed to create listener socket" << std::endl;
        return false;
    }
    
    // Set socket options for quick reuse
    unsigned long optval = 1;
    ioctlsocket(listen_socket_, FIONBIO, &optval);
    
    // Bind to all interfaces on specified port
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listen_socket_, reinterpret_cast<sockaddr*>(&server_addr), 
             sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed: " << WSAGetLastError() << std::endl;
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
        return false;
    }
    
    // Listen for connections
    if (listen(listen_socket_, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Listen failed: " << WSAGetLastError() << std::endl;
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
        return false;
    }
    
    running_.store(true, std::memory_order_release);
    should_stop_.store(false, std::memory_order_release);
    
    accept_thread_ = std::thread(&Listener::accept_loop, this);
    
    std::cout << "Server listening on port " << port << std::endl;
    return true;
}

void Listener::stop() {
    if (!running_.exchange(false, std::memory_order_release)) {
        return;
    }
    
    should_stop_.store(true, std::memory_order_release);
    
    // Wake up accept loop by closing socket
    if (listen_socket_ != INVALID_SOCKET) {
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
    }
    
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
}

void Listener::set_connected_callback(ClientConnectedCallback callback) {
    connected_callback_ = std::move(callback);
}

void Listener::set_disconnected_callback(ClientDisconnectedCallback callback) {
    disconnected_callback_ = std::move(callback);
}

void Listener::accept_loop() {
    while (!should_stop_.load(std::memory_order_acquire) && 
           running_.load(std::memory_order_acquire)) {
        
        sockaddr_in client_addr{};
        int client_addr_len = sizeof(client_addr);
        
        SOCKET client_socket = accept(listen_socket_, 
                                     reinterpret_cast<sockaddr*>(&client_addr), 
                                     &client_addr_len);
        
        if (client_socket == INVALID_SOCKET) {
            if (should_stop_.load(std::memory_order_acquire)) {
                break;
            }
            
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                std::cerr << "Accept failed: " << error << std::endl;
                Sleep(100);
            }
            continue;
        }
        
        // Client connected
        if (connected_callback_) {
            connected_callback_(client_socket);
        }
        
        // Handle the client in a separate thread
        std::thread client_thread(&Listener::handle_client, this, client_socket);
        client_thread.detach();  // Let thread manage its own lifecycle
    }
    
    std::cout << "Listener stopped" << std::endl;
}

void Listener::handle_client(SOCKET client_socket) {
    std::cout << "Handling new client connection" << std::endl;
    
    try {
        // Process incoming messages here
        std::vector<uint8_t> buffer(65536);
        char data[buffer.size()];
        
        while (running_.load(std::memory_order_acquire)) {
            int received = recv(client_socket, data, static_cast<int>(buffer.size()), 0);
            
            if (received <= 0) {
                if (received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                    Sleep(10);
                    continue;
                }
                
                // Connection closed
                break;
            }
            
            // TODO: Process message and forward to tunnel manager or display renderer
        }
    } catch (const std::exception& e) {
        std::cerr << "Error handling client: " << e.what() << std::endl;
    }
    
    // Client disconnected
    if (disconnected_callback_) {
        disconnected_callback_(client_socket);
    }
    
    closesocket(client_socket);
    std::cout << "Client disconnected" << std::endl;
}
