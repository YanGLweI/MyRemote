#pragma once

#include <QObject>

#include <cstdint>
#include <mutex>
#include <string>

#include "frame_pipeline.hpp"

class DisplayRenderer;
class TunnelManager;

// Drives one active control session over an existing client tunnel.
class RemoteController : public QObject {
    Q_OBJECT

public:
    RemoteController(TunnelManager& tunnels, DisplayRenderer& renderer,
                     QObject* parent = nullptr);

    bool start_control(const std::string& device_id);
    void request_control(const std::string& device_id, const std::string& password);
    void stop_control();

    bool is_controlling() const;
    // max_encode_width: 0 keeps whatever the device itself is configured for.
    void apply_quality(uint8_t fps, uint16_t bitrate_kbps,
                       uint16_t max_encode_width = 0);
    std::string controlled_device() const;

signals:
    void control_started(QString device_id);
    void control_stopped();
    void control_denied(QString device_id);
    void fps_updated(int net_fps, int decoded_fps);

public slots:
    // Connected directly: runs on the tunnel session thread and only hands the
    // payload to the decode pipeline.
    void on_video_frame(QString device_id, QByteArray payload);
    void on_mouse_moved(int x, int y);
    void on_mouse_button(int button, bool pressed);
    void on_mouse_wheel(int delta);
    void on_key(int vk, bool pressed, bool extended);
    void on_auth_result(QString device_id, bool ok);

private:
    bool do_start(const std::string& device_id);
    void set_controlled(const std::string& device_id);

    TunnelManager& tunnels_;
    DisplayRenderer& renderer_;
    FramePipeline pipeline_;
    mutable std::mutex state_mutex_;
    std::string controlled_;
    std::string pending_device_;
    uint8_t fps_ = 30;
    uint16_t bitrate_kbps_ = 2048;
    uint16_t max_encode_width_ = 0;
};
