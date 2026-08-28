#include "remote_controller.hpp"

#include <QTimer>

#include "display_renderer.hpp"
#include "log.hpp"
#include "messages.hpp"
#include "tunnel_manager.hpp"

RemoteController::RemoteController(TunnelManager& tunnels, DisplayRenderer& renderer,
                                   QObject* parent)
    : QObject(parent), tunnels_(tunnels), renderer_(renderer), pipeline_(renderer) {
    // Decoding happens on the pipeline thread; the GUI thread only gets a
    // cheap "something new to paint" nudge.
    connect(&pipeline_, &FramePipeline::frame_ready, this,
            [this] { renderer_.update(); });

    auto* fps_timer = new QTimer(this);
    connect(fps_timer, &QTimer::timeout, this, [this]() {
        int net = static_cast<int>(tunnels_.exchange_video_frames_in());
        int dec = static_cast<int>(pipeline_.exchange_decoded());
        emit fps_updated(net, dec);
    });
    fps_timer->start(1000);
}

bool RemoteController::is_controlling() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return !controlled_.empty();
}

std::string RemoteController::controlled_device() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return controlled_;
}

void RemoteController::set_controlled(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    controlled_ = device_id;
}

bool RemoteController::do_start(const std::string& device_id) {
    if (is_controlling()) {
        mlog::warn("Already controlling " + controlled_device() +
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
    auto request_keyframe = [this, device_id] {
        tunnels_.send_to_device(device_id, proto::MessageType::RequestKeyframe);
    };
    if (!pipeline_.start(device_id, width, height, request_keyframe)) {
        return false;
    }
    renderer_.set_remote_size(width, height);

    auto payload = proto::make_start_stream_payload(fps_, bitrate_kbps_,
                                                   max_encode_width_);
    if (!tunnels_.send_to_device(device_id, proto::MessageType::StartStream, payload)) {
        mlog::error("Failed to send StartStream to " + device_id);
        pipeline_.stop();
        return false;
    }
    request_keyframe();

    set_controlled(device_id);
    tunnels_.exchange_video_frames_in();
    mlog::info("Control session started: " + device_id);
    emit control_started(QString::fromStdString(device_id));
    return true;
}

void RemoteController::stop_control() {
    std::string device_id;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (controlled_.empty()) {
            return;
        }
        device_id.swap(controlled_);
    }
    tunnels_.send_to_device(device_id, proto::MessageType::StopStream);
    pipeline_.stop();
    renderer_.clear_frame();
    mlog::info("Control session stopped: " + device_id);
    emit control_stopped();
}

void RemoteController::on_video_frame(QString device_id, QByteArray payload) {
    if (device_id.toStdString() != controlled_device()) {
        return;  // frame from a device we are not controlling
    }
    pipeline_.push(std::move(payload));
}

void RemoteController::on_mouse_moved(int x, int y) {
    std::string device = controlled_device();
    if (device.empty()) return;
    tunnels_.send_to_device(device, proto::MessageType::InputEvent,
                            proto::make_mouse_move(static_cast<uint16_t>(x),
                                                   static_cast<uint16_t>(y)));
}

void RemoteController::on_mouse_button(int button, bool pressed) {
    std::string device = controlled_device();
    if (device.empty()) return;
    tunnels_.send_to_device(device, proto::MessageType::InputEvent,
                            proto::make_mouse_button(static_cast<uint8_t>(button),
                                                     pressed));
}

void RemoteController::on_mouse_wheel(int delta) {
    std::string device = controlled_device();
    if (device.empty()) return;
    tunnels_.send_to_device(device, proto::MessageType::InputEvent,
                            proto::make_mouse_wheel(static_cast<int16_t>(delta)));
}

void RemoteController::on_key(int vk, bool pressed, bool extended) {
    std::string device = controlled_device();
    if (device.empty()) return;
    tunnels_.send_to_device(device, proto::MessageType::InputEvent,
                            proto::make_key(static_cast<uint16_t>(vk), pressed,
                                            extended));
}

void RemoteController::apply_quality(uint8_t fps, uint16_t bitrate_kbps,
                                     uint16_t max_encode_width) {
    fps_ = fps;
    bitrate_kbps_ = bitrate_kbps;
    max_encode_width_ = max_encode_width;
    std::string device = controlled_device();
    if (!device.empty()) {
        tunnels_.send_to_device(device, proto::MessageType::StartStream,
                                proto::make_start_stream_payload(
                                    fps_, bitrate_kbps_, max_encode_width_));
    }
}

bool RemoteController::start_control(const std::string& device_id) {
    return do_start(device_id);
}

void RemoteController::request_control(const std::string& device_id,
                                       const std::string& password) {
    if (is_controlling()) {
        mlog::warn("Already controlling a device");
        return;
    }
    pending_device_ = device_id;
    tunnels_.begin_auth(device_id, password);
}

void RemoteController::on_auth_result(QString device_id, bool ok) {
    if (device_id.toStdString() != pending_device_) {
        return;
    }
    pending_device_.clear();
    if (ok) {
        do_start(device_id.toStdString());
    } else {
        mlog::warn("Control authorization failed for " + device_id.toStdString());
        emit control_denied(device_id);
    }
}
