#pragma once

#include <winsock2.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

// Passive TCP listener. The server only ever accepts connections that
// clients initiate; it never connects outward (one-way network rule).
class Listener {
public:
    using ClientConnectedCallback = std::function<void(SOCKET)>;

    Listener() = default;
    ~Listener();

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    bool start(const std::string& bind_address, int port);
    void stop();
    bool is_running() const { return running_.load(); }

    void set_connected_callback(ClientConnectedCallback callback) {
        connected_callback_ = std::move(callback);
    }

private:
    void accept_loop();
    static void ensure_winsock();

    SOCKET listen_socket_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::atomic<bool> should_stop_{false};
    std::thread accept_thread_;
    ClientConnectedCallback connected_callback_;
};
