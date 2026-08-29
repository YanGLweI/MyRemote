#pragma once

#include <QObject>

#include <cstdint>
#include <mutex>
#include <string>

#include "frame_pipeline.hpp"

class DisplayRenderer;
class QTimer;
class TunnelManager;

// One entry per picker row. `label` is what the selector shows and `spec` is the
// numbers behind it, shown as the item's tooltip. max_encode_width 0 means
// "whatever the device's own preset says", which is what the sharpest tier
// deliberately does.
struct QualityPreset {
    const char* label;
    const char* spec;
    uint8_t fps;
    uint16_t bitrate_kbps;
    uint16_t max_encode_width;
};

extern const QualityPreset kQualityPresets[];
extern const int kQualityPresetCount;
constexpr int kQualityDefault = 1;

// Drives one active control session over an existing client tunnel.
class RemoteController : public QObject {
    Q_OBJECT

public:
    RemoteController(TunnelManager& tunnels, DisplayRenderer& renderer,
                     QObject* parent = nullptr);
    ~RemoteController() override;

    void request_control(const std::string& device_id, const std::string& password);
    // Ends the session and forgets the device: the operator asked for this.
    void stop_control();
    // Same teardown, but stays ready to pick the session back up when the same
    // device reappears. A service-managed host restarts in about two seconds,
    // and making the operator re-authenticate for that is not their problem.
    void suspend_control();

    bool is_controlling() const;
    // max_encode_width: 0 keeps whatever the device itself is configured for.
    void apply_quality(uint8_t fps, uint16_t bitrate_kbps,
                       uint16_t max_encode_width = 0);
    std::string controlled_device() const;
    // Hands the remote machine back to its own logon screen; only a
    // service-hosted agent can follow what happens next.
    void lock_workstation();
    bool controlled_supports_logon() const;

signals:
    void control_started(QString device_id);
    void control_stopped();
    void control_denied(QString device_id);
    // fps is what actually reached the screen in the last second. latency_ms is
    // how old that picture was when it became one, and is -1 while nothing can be
    // said: before the clocks are tied, or while the far side sends no frames.
    void stats_updated(int fps, int latency_ms);
    // One-line explanation for the status bar: what auto-resume is doing, or
    // why it stopped trying.
    void status_note(QString text);

public slots:
    // Connected directly: runs on the tunnel session thread and only hands the
    // payload to the decode pipeline.
    void on_video_frame(QString device_id, QByteArray payload);
    void on_mouse_moved(int x, int y);
    void on_mouse_button(int button, bool pressed);
    void on_mouse_wheel(int delta);
    void on_key(int vk, bool pressed, bool extended);
    void on_auth_result(QString device_id, bool ok);
    // Arrives on a session thread, so it is delivered here on the GUI thread: the
    // clock state below is plain members on purpose.
    void on_clock_pong(QString device_id, quint64 t0_us, quint64 t3_us,
                       quint64 agent_recv_us, quint64 agent_send_us);

private:
    bool do_start(const std::string& device_id);
    void set_controlled(const std::string& device_id);
    void disarm_auto_resume();
    bool device_is_online(const std::string& device_id) const;
    // Re-authenticate and restart streaming on the armed device, if the retry
    // budget still allows it.
    void resume();
    // Ask the far side what its clock says, unless it has already proved it does
    // not answer.
    void ask_clock();
    // Milliseconds the last second of pictures spent getting here, or -1 when
    // there is nothing honest to say.
    int latency_ms();

    TunnelManager& tunnels_;
    DisplayRenderer& renderer_;
    FramePipeline pipeline_;
    mutable std::mutex state_mutex_;
    std::string controlled_;
    std::string pending_device_;
    std::string pending_password_;
    // Only ever armed while the operator's own session is live or was cut by
    // the far end; stop_control() clears it, so a device can never take over
    // the view unasked. The password lives here and nowhere else.
    std::string auto_device_;
    std::string auto_password_;
    int auto_attempts_ = 0;
    QTimer* auto_window_timer_ = nullptr;
    uint8_t fps_ = 30;
    uint16_t bitrate_kbps_ = 2048;
    uint16_t max_encode_width_ = 0;
    // Tied by the Ping/Pong exchange: "agent clock minus ours". A frame's capture
    // stamp is only meaningful once that difference is known.
    int64_t clock_offset_us_ = 0;
    bool clock_synced_ = false;
    uint64_t ping_sent_us_ = 0;
    // An agent too old to answer must not be asked forever, and each unanswered
    // Ping costs its log a warning line.
    int ping_unanswered_ = 0;
};
