#include "tunnel_manager.hpp"

#include <ws2tcpip.h>

#include <chrono>

#include "log.hpp"

namespace {
constexpr long long kHeartbeatTimeoutMs = 3000;
constexpr long long kRegisterDeadlineMs = 10000;
constexpr int kRecvTimeoutMs = 500;

long long steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

TunnelManager::TunnelManager(std::string secret_key, int max_connections,
                             QObject* parent)
    : QObject(parent),
      secret_key_(std::move(secret_key)),
      max_connections_(max_connections),
      aes_(crypto::derive_key(secret_key_)) {
    listener_.set_connected_callback([this](SOCKET socket) {
        on_new_connection(socket);
    });
}

TunnelManager::~TunnelManager() {
    stop();
}

long long TunnelManager::now_ms() {
    return steady_now_ms();
}

bool TunnelManager::start(const std::string& bind_address, int port) {
    if (running_.load()) {
        return true;
    }
    if (!listener_.start(bind_address, port)) {
        return false;
    }
    running_.store(true);
    reaper_thread_ = std::thread(&TunnelManager::reaper_loop, this);
    return true;
}

void TunnelManager::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    listener_.stop();

    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        for (auto& [id, session] : sessions_) {
            session->alive.store(false);
            if (session->socket != INVALID_SOCKET) {
                closesocket(session->socket);
                session->socket = INVALID_SOCKET;
            }
        }
        sessions_.clear();
    }

    if (reaper_thread_.joinable()) {
        reaper_thread_.join();
    }
    mlog::info("TunnelManager stopped");
}

void TunnelManager::on_new_connection(SOCKET socket) {
    sockaddr_in addr{};
    int len = sizeof(addr);
    std::string peer_ip = "?";
    if (getpeername(socket, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        char buf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
        peer_ip = buf;
    }

    DWORD timeout = kRecvTimeoutMs;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::thread(&TunnelManager::session_loop, this, socket, peer_ip).detach();
}

void TunnelManager::session_loop(SOCKET socket, const std::string& peer_ip) {
    proto::FrameDecoder decoder;
    std::vector<uint8_t> read_buffer(65536);
    std::shared_ptr<ClientSession> session;
    long long register_deadline = steady_now_ms() + kRegisterDeadlineMs;

    // Phase 1: wait for the Register message.
    while (steady_now_ms() < register_deadline) {
        int received = recv(socket, reinterpret_cast<char*>(read_buffer.data()),
                            static_cast<int>(read_buffer.size()), 0);
        if (received > 0) {
            if (!decoder.feed(read_buffer.data(), static_cast<size_t>(received))) {
                break;
            }
            if (!decoder.has_frame()) {
                continue;
            }
            auto frame = decoder.pop_frame();
            if (frame.type != proto::MessageType::Encrypted) {
                mlog::warn("Unencrypted Register attempt from " + peer_ip +
                           " (type " + std::to_string(static_cast<int>(frame.type)) +
                           "), rejecting");
                break;
            }
            std::vector<uint8_t> plain;
            try {
                plain = aes_.decrypt(frame.payload);
            } catch (const crypto::CryptoError&) {
                mlog::warn("Register decryption failed from " + peer_ip +
                           " (wrong connection key), rejecting");
                break;
            }
            if (plain.empty() ||
                plain[0] != static_cast<uint8_t>(proto::MessageType::Register)) {
                mlog::warn("First encrypted message is not Register from " + peer_ip);
                break;
            }
            proto::RegisterInfo info;
            std::vector<uint8_t> register_payload(plain.begin() + 1, plain.end());
            if (!proto::parse_register_payload(register_payload, info)) {
                mlog::warn("Malformed Register payload from " + peer_ip);
                break;
            }
            session = std::make_shared<ClientSession>();
            session->socket = socket;
            session->peer_ip = peer_ip;
            session->connect_time = std::time(nullptr);
            session->last_heartbeat_ms.store(steady_now_ms());
            if (!register_session(session, info)) {
                session.reset();
                break;
            }
            break;
        } else if (received == 0) {
            break;
        } else {
            int err = WSAGetLastError();
            if (err != WSAETIMEDOUT && err != WSAEWOULDBLOCK) {
                break;
            }
        }
    }

    if (!session) {
        closesocket(socket);
        return;
    }

    // Phase 2: steady-state message pump.
    while (session->alive.load()) {
        int received = recv(socket, reinterpret_cast<char*>(read_buffer.data()),
                            static_cast<int>(read_buffer.size()), 0);
        if (received > 0) {
            if (!decoder.feed(read_buffer.data(), static_cast<size_t>(received))) {
                mlog::warn("Protocol violation from device " + session->device_id);
                break;
            }
            while (decoder.has_frame()) {
                auto frame = decoder.pop_frame();
                if (frame.type == proto::MessageType::Heartbeat) {
                    session->last_heartbeat_ms.store(steady_now_ms());
                    continue;
                }
                if (frame.type != proto::MessageType::Encrypted) {
                    mlog::warn("Unexpected plaintext frame from device " +
                               session->device_id);
                    continue;
                }
                std::vector<uint8_t> plain;
                try {
                    plain = aes_.decrypt(frame.payload);
                } catch (const crypto::CryptoError&) {
                    mlog::warn("Decryption failed for device " + session->device_id +
                               ", closing tunnel");
                    session->alive.store(false);
                    break;
                }
                if (plain.empty()) {
                    continue;
                }
                proto::FrameDecoder::Frame inner;
                inner.type = static_cast<proto::MessageType>(plain[0]);
                inner.payload.assign(plain.begin() + 1, plain.end());
                frame = std::move(inner);
                switch (frame.type) {
                    case proto::MessageType::VideoFrame:
                        emit video_frame_received(
                            QString::fromStdString(session->device_id),
                            QByteArray(reinterpret_cast<const char*>(
                                           frame.payload.data()),
                                       static_cast<int>(frame.payload.size())));
                        break;
                    default:
                        mlog::warn("Unhandled message type " +
                                  std::to_string(static_cast<int>(frame.type)) +
                                  " from device " + session->device_id);
                        break;
                }
            }
        } else if (received == 0) {
            mlog::info("Device " + session->device_id + " closed the connection");
            break;
        } else {
            int err = WSAGetLastError();
            if (err != WSAETIMEDOUT && err != WSAEWOULDBLOCK) {
                if (session->alive.load()) {
                    mlog::warn("recv() error for device " + session->device_id +
                              ": " + std::to_string(err));
                }
                break;
            }
        }
    }

    remove_session(session);
    if (session->socket != INVALID_SOCKET) {
        closesocket(session->socket);
        session->socket = INVALID_SOCKET;
    }
}

bool TunnelManager::register_session(const std::shared_ptr<ClientSession>& session,
                                     const proto::RegisterInfo& info) {
    proto::RegisterStatus status = proto::RegisterStatus::Ok;

    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        if (static_cast<int>(sessions_.size()) >= max_connections_) {
            status = proto::RegisterStatus::ServerFull;
        }
    }

    // Send the ack (encrypted) before installing the session.
    std::vector<uint8_t> ack = proto::make_register_ack_payload(status);
    std::vector<uint8_t> inner;
    inner.push_back(static_cast<uint8_t>(proto::MessageType::RegisterAck));
    inner.insert(inner.end(), ack.begin(), ack.end());
    std::vector<uint8_t> wire =
        proto::encode_frame(proto::MessageType::Encrypted, aes_.encrypt(inner));
    ::send(session->socket, reinterpret_cast<const char*>(wire.data()),
           static_cast<int>(wire.size()), 0);

    if (status != proto::RegisterStatus::Ok) {
        mlog::warn("Rejected registration for device " + info.device_id +
                  " (status=" + std::to_string(static_cast<int>(status)) + ")");
        return false;
    }

    session->device_id = info.device_id;
    session->device_name =
        info.device_name.empty() ? info.device_id : info.device_name;
    session->screen_width = info.screen_width;
    session->screen_height = info.screen_height;

    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        auto existing = sessions_.find(info.device_id);
        if (existing != sessions_.end()) {
            // Same device re-registering (reconnect): retire the old tunnel.
            auto old = existing->second;
            old->alive.store(false);
            if (old->socket != INVALID_SOCKET) {
                closesocket(old->socket);
                old->socket = INVALID_SOCKET;
            }
            sessions_.erase(existing);
        }
        sessions_[info.device_id] = session;
    }

    session->registered.store(true);
    mlog::info("Device registered: " + info.device_id + " (" + session->device_name +
              ") from " + session->peer_ip + ", " +
              std::to_string(session->screen_width) + "x" +
              std::to_string(session->screen_height));

    emit device_registered(QString::fromStdString(info.device_id),
                           QString::fromStdString(session->device_name),
                           info.screen_width, info.screen_height);
    return true;
}

void TunnelManager::remove_session(const std::shared_ptr<ClientSession>& session) {
    if (!session->registered.exchange(false)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        auto it = sessions_.find(session->device_id);
        if (it != sessions_.end() && it->second == session) {
            sessions_.erase(it);
        }
    }
    mlog::info("Device unregistered: " + session->device_id);
    emit device_unregistered(QString::fromStdString(session->device_id));
}

bool TunnelManager::send_to_device(const std::string& device_id,
                                   proto::MessageType type,
                                   const std::vector<uint8_t>& payload) {
    std::shared_ptr<ClientSession> session;
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        auto it = sessions_.find(device_id);
        if (it == sessions_.end()) {
            return false;
        }
        session = it->second;
    }
    if (!session->registered.load() || session->socket == INVALID_SOCKET) {
        return false;
    }

    std::vector<uint8_t> inner;
    inner.reserve(1 + payload.size());
    inner.push_back(static_cast<uint8_t>(type));
    inner.insert(inner.end(), payload.begin(), payload.end());
    std::vector<uint8_t> wire =
        proto::encode_frame(proto::MessageType::Encrypted, aes_.encrypt(inner));
    std::lock_guard<std::mutex> lock(session->send_mutex);
    size_t total = 0;
    while (total < wire.size()) {
        int sent = ::send(session->socket,
                          reinterpret_cast<const char*>(wire.data() + total),
                          static_cast<int>(wire.size() - total), 0);
        if (sent <= 0) {
            mlog::error("send_to_device(" + device_id + ") failed: " +
                       std::to_string(WSAGetLastError()));
            return false;
        }
        total += static_cast<size_t>(sent);
    }
    return true;
}

std::vector<TunnelManager::DeviceInfo> TunnelManager::online_devices() const {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    std::vector<DeviceInfo> result;
    result.reserve(sessions_.size());
    for (const auto& [id, session] : sessions_) {
        if (!session->registered.load()) {
            continue;
        }
        DeviceInfo info;
        info.device_id = session->device_id;
        info.device_name = session->device_name;
        info.peer_ip = session->peer_ip;
        info.screen_width = session->screen_width;
        info.screen_height = session->screen_height;
        info.connect_time = session->connect_time;
        result.push_back(std::move(info));
    }
    return result;
}

void TunnelManager::reaper_loop() {
    while (running_.load()) {
        Sleep(1000);
        long long now = steady_now_ms();

        std::vector<std::shared_ptr<ClientSession>> expired;
        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            for (const auto& [id, session] : sessions_) {
                if (session->registered.load() &&
                    now - session->last_heartbeat_ms.load() > kHeartbeatTimeoutMs) {
                    expired.push_back(session);
                }
            }
        }
        for (auto& session : expired) {
            mlog::warn("Device " + session->device_id +
                      " missed heartbeats, marking offline");
            session->alive.store(false);
            SOCKET sock = session->socket;
            session->socket = INVALID_SOCKET;
            if (sock != INVALID_SOCKET) {
                closesocket(sock);
            }
        }
    }
}
