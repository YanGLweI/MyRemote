#include "remote_controller.hpp"

#include <QTimer>

#include <chrono>

#include "display_renderer.hpp"
#include "log.hpp"
#include "messages.hpp"
#include "tunnel_manager.hpp"

namespace {
constexpr uint8_t kDefaultFps = 30;
constexpr uint16_t kDefaultBitrateKbps = 2048;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

RemoteController::RemoteController(TunnelManager& tunnels, DisplayRenderer& renderer,
                                   QObject* parent)
    : QObject(parent), tunnels_(tunnels), renderer_(renderer) {
    auto* fps_timer = new QTimer(this);
    connect(fps_timer, &QTimer::timeout, this, [this]() {
        uint64_t count = frames_received_.exchange(0);
        emit fps_updated(static_cast<float>(count));
    });
    fps_timer->start(1000);
}

bool RemoteController::start_control(const std::string& device_id) {
    if (controlled_.has_value()) {
        mlog::warn("Already controlling " + controlled_.value() +
                  "; stop it before switching devices");
        return false;
    }

    int width = 0;
    int height = 0;
    for (const auto& info : tunnels_.online_devices()) {
        if (info.device_id == device_id) {
            width = info.screen_width;
            height = info.screen_height;
            break;
        }
    }
    if (!decoder_.initialize(width, height)) {
        mlog::error("Decoder init failed for " + device_id);
        return false;
    }

    auto payload = proto::make_start_stream_payload(kDefaultFps, kDefaultBitrateKbps);
    if (!tunnels_.send_to_device(device_id, proto::MessageType::StartStream, payload)) {
        mlog::error("Failed to send StartStream to " + device_id);
        return false;
    }
    tunnels_.send_to_device(device_id, proto::MessageType::RequestKeyframe);

    controlled_ = device_id;
    frames_received_.store(0);
    last_good_frame_ms_.store(0);
    mlog::info("Control session started: " + device_id);
    emit control_started(QString::fromStdString(device_id));
    return true;
}

void RemoteController::stop_control() {
    if (!controlled_.has_value()) {
        return;
    }
    std::string device_id = controlled_.value();
    tunnels_.send_to_device(device_id, proto::MessageType::StopStream);
    controlled_.reset();
    renderer_.clear_frame();
    mlog::info("Control session stopped: " + device_id);
    emit control_stopped();
}

void RemoteController::on_video_frame(QString device_id, QByteArray payload) {
    if (!controlled_.has_value() ||
        device_id.toStdString() != controlled_.value()) {
        return;  // frame from a device we are not controlling
    }

    proto::VideoFrameInfo info;
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    if (!proto::parse_video_frame_payload(bytes, info)) {
        mlog::warn("Malformed VideoFrame payload");
        return;
    }

    frames_received_.fetch_add(1);

    QImage frame;
    if (decoder_.decode(info.data, info.size, frame)) {
        last_good_frame_ms_.store(now_ms());
        renderer_.set_frame(frame);
    } else if (now_ms() - last_good_frame_ms_.load() > 2000) {
        // Stalled (missing keyframe after loss): ask the client for one.
        last_good_frame_ms_.store(now_ms());
        tunnels_.send_to_device(controlled_.value(),
                                proto::MessageType::RequestKeyframe);
    }
}
