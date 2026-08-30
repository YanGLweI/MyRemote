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

// What the operator's roster row should say. A device is never dropped from the
// roster because its tunnel blinked: the agent is designed to come back, and a
// row that disappears and re-appears reads as a broken console.
enum class DeviceState : int {
    Live = 0,         // registered tunnel right now
    Reconnecting = 1, // tunnel gone moments ago, expecting it back
    Offline = 2,      // still not back; last known details only
};

struct ClientSession {
    SOCKET socket = INVALID_SOCKET;
    std::string device_id;
    std::string device_name;
    std::string peer_ip;
    uint16_t screen_width = 0;
    uint16_t screen_height = 0;
    bool elevated = false;
    bool elevation_known = false;
    uint8_t flags = 0;  // proto::kRegisterFlag* / kFlag*
    time_t connect_time = 0;
    std::atomic<long long> last_heartbeat_ms{0};
    std::atomic<bool> registered{false};
    std::atomic<bool> alive{true};
    // Network arrival rate for this tunnel only, so each open session can
    // report its own NET fps.
    std::atomic<uint64_t> frames_in{0};
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
        bool elevated = false;
        bool elevation_known = false;
        uint8_t flags = 0;  // proto::kRegisterFlag* / kFlag*
        time_t connect_time = 0;
        DeviceState state = DeviceState::Offline;
        long long last_seen_ms = 0;  // steady clock of the last proof of life
        time_t last_seen_time = 0;   // wall clock, for "上次 HH:MM" in the roster
    };

    TunnelManager(std::string secret_key, int max_connections,
                  QObject* parent = nullptr);
    ~TunnelManager() override;

    bool start(const std::string& bind_address, int port);
    void stop();

    bool send_to_device(const std::string& device_id, proto::MessageType type,
                        const std::vector<uint8_t>& payload = {});

    // Secondary control-password challenge (P2).
    void begin_auth(const std::string& device_id, const std::string& password);
    void disconnect_device(const std::string& device_id);

    std::vector<DeviceInfo> online_devices() const;
    // Everything seen this run, live or not: the roster's source of truth.
    std::vector<DeviceInfo> roster() const;
    // One lookup instead of the scan callers used to do over online_devices(),
    // which no longer holds a device during the reconnect window.
    bool roster_for(const std::string& device_id, DeviceInfo* out) const;

    // Video frames forwarded from this device's session thread since the last
    // call: the network arrival rate, independent of GUI decode speed.
    uint64_t exchange_frames_in(const std::string& device_id);

signals:
    void device_registered(QString device_id, QString device_name, int width,
                           int height);
    // int is a DeviceState. Never emitted just to say "delete this row".
    void device_state_changed(QString device_id, int state);
    void video_frame_received(QString device_id, QByteArray payload);
    void auth_result(QString device_id, bool ok);
    // One round trip. The agent echoes our own stamp back unchanged, so both
    // samples are this machine's clock and nothing has to be compared across
    // two machines. t3 is taken on the session thread on purpose - the queued
    // hop to the GUI thread is scheduling, not delay on the wire, and counting
    // it would inflate every reading.
    void pong(QString device_id, quint64 t0_us, quint64 t3_us);
    // The agent's answer to a mode query, and its acknowledgement after every
    // set attempt: raw DisplayModes payload, parsed by the controller.
    void display_modes(QString device_id, QByteArray payload);

private:
    void on_new_connection(SOCKET socket);
    void session_loop(SOCKET socket, const std::string& peer_ip);
    bool register_session(const std::shared_ptr<ClientSession>& session,
                          const proto::RegisterInfo& info);
    void remove_session(const std::shared_ptr<ClientSession>& session);
    void reaper_loop();
    // Caller holds pool_mutex_. Records what the operator saw last so the row
    // survives the tunnel, and returns the previous state for change detection.
    DeviceState note_state_locked(const std::string& device_id, DeviceState state);
    // A resize arrives on the tunnel thread, long after the registration that
    // filled this row. Every reader goes through the roster, so it has to carry
    // the live geometry: the header, and the size a new session seeds with.
    void note_geometry(const std::string& device_id, uint16_t width, uint16_t height);
    static long long now_ms();

    Listener listener_;
    std::string secret_key_;
    int max_connections_;

    mutable std::mutex pool_mutex_;
    std::unordered_map<std::string, std::shared_ptr<ClientSession>> sessions_;
    // device_id -> last known details + state, kept across reconnects.
    std::unordered_map<std::string, DeviceInfo> roster_;

    std::atomic<bool> running_{false};
    std::thread reaper_thread_;
    crypto::AesGcm aes_;
    std::mutex auth_mutex_;
    std::unordered_map<std::string, std::pair<std::vector<uint8_t>, std::string>>
        pending_auth_;
};
