#pragma once

#include <QObject>

#include <atomic>
#include <optional>
#include <string>

#include "h264_decoder.hpp"

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

    bool is_controlling() const { return controlled_.has_value(); }
    void apply_quality(uint8_t fps, uint16_t bitrate_kbps);
    std::string controlled_device() const {
        return controlled_.value_or(std::string());
    }

signals:
    void control_started(QString device_id);
    void control_stopped();
    void control_denied(QString device_id);
    void fps_updated(float fps);

public slots:
    void on_video_frame(QString device_id, QByteArray payload);
    void on_mouse_moved(int x, int y);
    void on_mouse_button(int button, bool pressed);
    void on_mouse_wheel(int delta);
    void on_key(int vk, bool pressed, bool extended);
    void on_auth_result(QString device_id, bool ok);

private:
    bool do_start(const std::string& device_id);
    TunnelManager& tunnels_;
    DisplayRenderer& renderer_;
    H264Decoder decoder_;
    std::optional<std::string> controlled_;
    std::string pending_device_;
    uint8_t fps_ = 30;
    uint16_t bitrate_kbps_ = 2048;
    std::atomic<uint64_t> frames_received_{0};
    std::atomic<long long> last_good_frame_ms_{0};
};
