#include "remote_controller.hpp"

#include "display_renderer.hpp"
#include "log.hpp"
#include "messages.hpp"
#include "tunnel_manager.hpp"

namespace {
constexpr uint8_t kDefaultFps = 30;
constexpr uint16_t kDefaultBitrateKbps = 2048;
}  // namespace

RemoteController::RemoteController(TunnelManager& tunnels, DisplayRenderer& renderer,
                                   QObject* parent)
    : QObject(parent), tunnels_(tunnels), renderer_(renderer) {
    decoder_.initialize();
}

bool RemoteController::start_control(const std::string& device_id) {
    if (controlled_.has_value()) {
        mlog::warn("Already controlling " + controlled_.value() +
                  "; stop it before switching devices");
        return false;
    }

    auto payload = proto::make_start_stream_payload(kDefaultFps, kDefaultBitrateKbps);
    if (!tunnels_.send_to_device(device_id, proto::MessageType::StartStream, payload)) {
        mlog::error("Failed to send StartStream to " + device_id);
        return false;
    }

    controlled_ = device_id;
    frames_received_.store(0);
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

    uint64_t count = frames_received_.fetch_add(1) + 1;
    if (count % 300 == 1) {
        mlog::info("Video stream from " + controlled_.value() + ": frame #" +
                  std::to_string(count) + ", " + std::to_string(info.size) +
                  " bytes" + (info.is_keyframe ? " [keyframe]" : ""));
    }

    QImage frame;
    if (decoder_.decode(info.data, info.size, frame)) {
        renderer_.set_frame(frame);
    }
    // TODO(M4): decoded frames reach the renderer here.
}
