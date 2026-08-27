#pragma once

#include <winsock2.h>

#include <QObject>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "crypto.hpp"
#include "frame_codec.hpp"
#include "listener.hpp"
#include "messages.hpp"

struct ClientSession {
    SOCKET socket = INVALID_SOCKET;
    std::string device_id;
    std::string device_name;
    std::string peer_ip;
    uint16_t screen_width = 0;
    uint16_t screen_height = 0;
    time_t connect_time = 0;
    std::atomic<long long> last_heartbeat_ms{0};
    std::atomic<bool> registered{false};
    std::atomic<bool> alive{true};
    std::mutex send_mutex;
};

// Owns all client tunnels. Every byte flows through connections that the
// clients initiated; nothing here ever dials out.
class TunnelManager : public QObject {
    Q_OBJECT

public:
    struct DeviceInfo {
        std::string device_id;
        std::string device_name;
        std::string peer_ip;
        uint16_t screen_width = 0;
        uint16_t screen_height = 0;
        time_t connect_time = 0;
    };

    TunnelManager(std::string secret_key, int max_connections,
                  QObject* parent = nullptr);
    ~TunnelManager() override;

    bool start(const std::string& bind_address, int port);
    void stop();

    bool send_to_device(const std::string& device_id, proto::MessageType type,
                        const std::vector<uint8_t>& payload = {});

    std::vector<DeviceInfo> online_devices() const;

signals:
    void device_registered(QString device_id, QString device_name, int width,
                           int height);
    void device_unregistered(QString device_id);
    void video_frame_received(QString device_id, QByteArray payload);

private:
    void on_new_connection(SOCKET socket);
    void session_loop(SOCKET socket, const std::string& peer_ip);
    bool register_session(const std::shared_ptr<ClientSession>& session,
                          const proto::RegisterInfo& info);
    void remove_session(const std::shared_ptr<ClientSession>& session);
    void reaper_loop();
    static long long now_ms();

    Listener listener_;
    std::string secret_key_;
    int max_connections_;

    mutable std::mutex pool_mutex_;
    std::unordered_map<std::string, std::shared_ptr<ClientSession>> sessions_;

    std::atomic<bool> running_{false};
    std::thread reaper_thread_;
    crypto::AesGcm aes_;
};
