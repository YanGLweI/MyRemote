#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "frame_codec.hpp"
#include "messages.hpp"

// Client-side outbound TCP connection to the server.
// This is the ONLY network path in the client; the client never listens.
class Connection {
public:
    using MessageCallback =
        std::function<void(proto::MessageType, std::vector<uint8_t>)>;

    Connection();
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool connect(const std::string& server_ip, int port, int timeout_sec = 10);
    void disconnect();
    bool is_connected() const { return connected_.load(); }

    bool send(proto::MessageType type, const std::vector<uint8_t>& payload = {});

    void set_message_callback(MessageCallback callback) {
        message_callback_ = std::move(callback);
    }

private:
    void receive_loop();
    static void ensure_winsock();

    SOCKET socket_ = INVALID_SOCKET;
    std::thread recv_thread_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> should_stop_{false};
    std::mutex send_mutex_;
    MessageCallback message_callback_;
};
