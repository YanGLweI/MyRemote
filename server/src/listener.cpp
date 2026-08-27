#include "listener.hpp"

#include <ws2tcpip.h>

#include <mutex>

#include "log.hpp"

Listener::~Listener() {
    stop();
}

void Listener::ensure_winsock() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
    });
}

bool Listener::start(const std::string& bind_address, int port) {
    if (running_.load()) {
        return true;
    }
    ensure_winsock();

    listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket_ == INVALID_SOCKET) {
        mlog::error("Listener socket creation failed: " + std::to_string(WSAGetLastError()));
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    if (bind_address.empty() || bind_address == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
        mlog::error("Invalid bind address: " + bind_address);
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
        return false;
    }

    if (bind(listen_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
        SOCKET_ERROR) {
        mlog::error("bind() failed: " + std::to_string(WSAGetLastError()));
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
        return false;
    }

    if (listen(listen_socket_, SOMAXCONN) == SOCKET_ERROR) {
        mlog::error("listen() failed: " + std::to_string(WSAGetLastError()));
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
        return false;
    }

    should_stop_.store(false);
    running_.store(true);
    accept_thread_ = std::thread(&Listener::accept_loop, this);
    mlog::info("Server listening on " + bind_address + ":" + std::to_string(port));
    return true;
}

void Listener::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    should_stop_.store(true);

    SOCKET sock = listen_socket_;
    listen_socket_ = INVALID_SOCKET;
    if (sock != INVALID_SOCKET) {
        closesocket(sock);  // unblocks accept()
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    mlog::info("Listener stopped");
}

void Listener::accept_loop() {
    while (!should_stop_.load()) {
        sockaddr_in client_addr{};
        int addr_len = sizeof(client_addr);
        SOCKET client = accept(listen_socket_,
                               reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client == INVALID_SOCKET) {
            if (should_stop_.load()) {
                break;
            }
            int err = WSAGetLastError();
            if (err != WSAEINTR && err != WSAECONNRESET) {
                mlog::error("accept() failed: " + std::to_string(err));
                Sleep(200);
            }
            continue;
        }

        char ip_str[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        mlog::info(std::string("Incoming connection from ") + ip_str);

        if (connected_callback_) {
            connected_callback_(client);
        } else {
            closesocket(client);
        }
    }
}
