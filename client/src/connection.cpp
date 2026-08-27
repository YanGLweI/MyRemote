#include "connection.hpp"

#include <windows.h>

#include <cstring>

#include "log.hpp"

Connection::Connection() {
    ensure_winsock();
}

Connection::~Connection() {
    disconnect();
}

void Connection::ensure_winsock() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
    });
}

bool Connection::connect(const std::string& server_ip, int port, int timeout_sec) {
    if (connected_.load()) {
        return true;
    }

    // Reap the previous session's receive thread before reusing the slot;
    // assigning to a joinable std::thread would terminate the process.
    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }

    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET) {
        mlog::error("socket() failed: " + std::to_string(WSAGetLastError()));
        return false;
    }

    // Blocking connect with SO_SNDTIMEO/timeout via select on non-blocking socket.
    u_long non_blocking = 1;
    ioctlsocket(socket_, FIONBIO, &non_blocking);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    if (inet_pton(AF_INET, server_ip.c_str(), &addr.sin_addr) != 1) {
        mlog::error("Invalid server IP: " + server_ip);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }

    int rc = ::connect(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            mlog::error("connect() failed: " + std::to_string(WSAGetLastError()));
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(socket_, &write_fds);
        timeval tv{timeout_sec, 0};
        if (select(0, nullptr, &write_fds, nullptr, &tv) <= 0) {
            mlog::error("connect() timed out after " + std::to_string(timeout_sec) + "s");
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }
        int sock_err = 0;
        int err_len = sizeof(sock_err);
        getsockopt(socket_, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&sock_err), &err_len);
        if (sock_err != 0) {
            mlog::error("connect() async error: " + std::to_string(sock_err));
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }
    }

    // Back to blocking mode with a receive timeout so the recv loop can
    // notice shutdown requests.
    u_long blocking = 0;
    ioctlsocket(socket_, FIONBIO, &blocking);
    DWORD recv_timeout_ms = 500;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&recv_timeout_ms), sizeof(recv_timeout_ms));

    should_stop_.store(false);
    connected_.store(true);
    recv_thread_ = std::thread(&Connection::receive_loop, this);

    mlog::info("Connected to " + server_ip + ":" + std::to_string(port));
    return true;
}

void Connection::disconnect() {
    if (!connected_.exchange(false)) {
        return;
    }
    should_stop_.store(true);

    SOCKET sock = socket_;
    socket_ = INVALID_SOCKET;
    if (sock != INVALID_SOCKET) {
        shutdown(sock, SD_BOTH);
        closesocket(sock);
    }

    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }
    mlog::info("Disconnected from server");
}

bool Connection::send(proto::MessageType type, const std::vector<uint8_t>& payload) {
    if (!connected_.load() || socket_ == INVALID_SOCKET) {
        return false;
    }

    proto::MessageType wire_type = type;
    std::vector<uint8_t> wire_payload = payload;
    if (aes_ && type != proto::MessageType::Heartbeat) {
        std::vector<uint8_t> inner;
        inner.reserve(1 + payload.size());
        inner.push_back(static_cast<uint8_t>(type));
        inner.insert(inner.end(), payload.begin(), payload.end());
        wire_payload = aes_->encrypt(inner);
        wire_type = proto::MessageType::Encrypted;
    }

    std::vector<uint8_t> frame = proto::encode_frame(wire_type, wire_payload);

    std::lock_guard<std::mutex> lock(send_mutex_);
    size_t total = 0;
    while (total < frame.size()) {
        int sent = ::send(socket_, reinterpret_cast<const char*>(frame.data() + total),
                          static_cast<int>(frame.size() - total), 0);
        if (sent <= 0) {
            mlog::error("send() failed: " + std::to_string(WSAGetLastError()));
            connected_.store(false);
            return false;
        }
        total += static_cast<size_t>(sent);
    }
    return true;
}

void Connection::receive_loop() {
    proto::FrameDecoder decoder;
    std::vector<uint8_t> read_buffer(65536);

    while (!should_stop_.load()) {
        SOCKET sock = socket_;
        if (sock == INVALID_SOCKET) {
            break;
        }
        int received = recv(sock, reinterpret_cast<char*>(read_buffer.data()),
                            static_cast<int>(read_buffer.size()), 0);
        if (received > 0) {
            if (!decoder.feed(read_buffer.data(), static_cast<size_t>(received))) {
                mlog::error("Protocol violation from server, closing connection");
                break;
            }
            while (decoder.has_frame()) {
                auto frame = decoder.pop_frame();
                proto::MessageType type = frame.type;
                std::vector<uint8_t> payload = std::move(frame.payload);
                if (type == proto::MessageType::Encrypted) {
                    if (!aes_) {
                        mlog::error("Encrypted frame received but no key configured");
                        break;
                    }
                    try {
                        auto plain = aes_->decrypt(payload);
                        if (plain.empty()) {
                            throw crypto::CryptoError("empty plaintext");
                        }
                        type = static_cast<proto::MessageType>(plain[0]);
                        payload.assign(plain.begin() + 1, plain.end());
                    } catch (const crypto::CryptoError&) {
                        mlog::warn("Decryption failed (wrong key or tampering), closing");
                        break;
                    }
                }
                if (message_callback_) {
                    message_callback_(type, std::move(payload));
                }
            }
        } else if (received == 0) {
            mlog::warn("Server closed the connection");
            break;
        } else {
            int err = WSAGetLastError();
            if (err != WSAETIMEDOUT && err != WSAEWOULDBLOCK) {
                if (!should_stop_.load()) {
                    mlog::error("recv() failed: " + std::to_string(err));
                }
                break;
            }
        }
    }

    connected_.store(false);
}
