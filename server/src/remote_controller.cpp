#include "remote_controller.hpp"

#include <QTimer>

#include "display_renderer.hpp"
#include "log.hpp"
#include "messages.hpp"
#include "tunnel_manager.hpp"

const QualityPreset kQualityPresets[] = {
    {"流畅", "30fps · 1.5M · 720p", 30, 1500, 1280},
    {"均衡", "30fps · 2M · 1080p", 30, 2048, 1920},
    {"清晰", "60fps · 6M · 原画", 60, 6000, 0},
};
const int kQualityPresetCount =
    static_cast<int>(sizeof(kQualityPresets) / sizeof(kQualityPresets[0]));

namespace {

// An agent that has never answered will not start answering: it is older than
// this message. Asking again would also cost its log a warning every second.
constexpr int kPingGiveUp = 5;
// Only one Ping is ever outstanding, so a reply may take as long as the machine
// needs - being late is not a reason to throw the measurement away. Past this
// long the Ping counts as lost, and an answer that arrives any later has nothing
// left to be compared against.
constexpr int64_t kPingTimeoutUs = 3000000;
// How often to keep probing once the budget is spent: enough to notice a peer
// that starts answering, slow enough not to matter to one that never will.
constexpr int kPingSlowRetry = 10;

}  // namespace

RemoteController::RemoteController(TunnelManager& tunnels, DisplayRenderer& renderer,
                                   QObject* parent)
    : QObject(parent), tunnels_(tunnels), renderer_(renderer), pipeline_(renderer) {
    // Decoding happens on the pipeline thread; the GUI thread only gets a
    // cheap "something new to paint" nudge.
    connect(&pipeline_, &FramePipeline::frame_ready, this,
            [this] { renderer_.update(); });

    // The agent re-reports its geometry whenever the desktop resizes, and
    // re-registers when the service relaunches its host. Both need the same
    // answer from here: stop trusting the frame we are holding.
    connect(&tunnels_, &TunnelManager::device_registered, this,
            [this](QString device_id, QString, int width, int height) {
                const std::string id = device_id.toStdString();
                if (id != controlled_device()) {
                    // The operator's session was cut by the far end; pick it
                    // back up. Delayed because the registration arrives a beat
                    // before the agent is ready to stream.
                    if (!id.empty() && id == auto_device_ && !is_controlling()) {
                        QTimer::singleShot(700, this, [this, id] { resume(); });
                    }
                    return;
                }
                if (width <= 0 || height <= 0) return;
                renderer_.set_remote_size(width, height);
                renderer_.clear_frame();
                tunnels_.send_to_device(id, proto::MessageType::RequestKeyframe);
            });

    // Reopens the retry window so a host that comes back broken cannot be
    // re-authenticated forever.
    auto_window_timer_ = new QTimer(this);
    auto_window_timer_->setSingleShot(true);
    auto_window_timer_->setInterval(60000);
    connect(auto_window_timer_, &QTimer::timeout, this,
            [this] { auto_attempts_ = 0; });

    connect(&tunnels_, &TunnelManager::pong, this, &RemoteController::on_pong);

    auto* stats_timer = new QTimer(this);
    connect(stats_timer, &QTimer::timeout, this, [this]() {
        const int decoded = static_cast<int>(pipeline_.exchange_decoded());
        ping();
        emit stats_updated(decoded, rtt_ms_);
    });
    stats_timer->start(1000);
}

RemoteController::~RemoteController() {
    disarm_auto_resume();
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
    tunnels_.exchange_frames_in(device_id);
    // A round trip measured on the previous session says nothing about this one,
    // and the device may have rebooted since.
    rtt_ms_ = -1;
    ping_sent_us_ = 0;
    ping_unanswered_ = 0;
    mlog::info("Control session started: " + device_id);
    emit control_started(QString::fromStdString(device_id));
    return true;
}

void RemoteController::stop_control() {
    disarm_auto_resume();
    suspend_control();
}

void RemoteController::suspend_control() {
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

void RemoteController::disarm_auto_resume() {
    auto_attempts_ = 0;
    if (auto_window_timer_) {
        auto_window_timer_->stop();
    }
    auto_device_.clear();
    auto_password_.clear();
}

bool RemoteController::device_is_online(const std::string& device_id) const {
    for (const auto& info : tunnels_.online_devices()) {
        if (info.device_id == device_id) {
            return true;
        }
    }
    return false;
}

void RemoteController::resume() {
    if (auto_device_.empty() || is_controlling()) {
        return;
    }
    if (!device_is_online(auto_device_)) {
        return;  // The next registration triggers another attempt.
    }
    if (auto_attempts_ >= 5) {
        mlog::warn("Giving up on auto-resume for " + auto_device_);
        emit status_note(QStringLiteral("自动恢复失败，请重新双击设备"));
        disarm_auto_resume();
        return;
    }
    ++auto_attempts_;
    if (!auto_window_timer_->isActive()) {
        auto_window_timer_->start();
    }
    mlog::info("Auto-resuming the control session on " + auto_device_ +
               " (attempt " + std::to_string(auto_attempts_) + ")");
    emit status_note(QStringLiteral("对端已重启，正在自动恢复画面…"));
    request_control(auto_device_, auto_password_);
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

void RemoteController::lock_workstation() {
    std::string device = controlled_device();
    if (device.empty()) {
        mlog::warn("Logon screen requested with no active session");
        return;
    }
    mlog::info("Requesting the logon screen on " + device);
    tunnels_.send_to_device(device, proto::MessageType::LockWorkstation);
}

bool RemoteController::controlled_supports_logon() const {
    std::string device = controlled_device();
    if (device.empty()) {
        return false;
    }
    for (const auto& info : tunnels_.online_devices()) {
        if (info.device_id == device) {
            return (info.flags & proto::kFlagServiceHost) != 0;
        }
    }
    return false;
}

void RemoteController::request_control(const std::string& device_id,
                                       const std::string& password) {
    if (is_controlling()) {
        mlog::warn("Already controlling a device");
        return;
    }
    pending_device_ = device_id;
    pending_password_ = password;
    tunnels_.begin_auth(device_id, password);
}

void RemoteController::on_auth_result(QString device_id, bool ok) {
    if (device_id.toStdString() != pending_device_) {
        return;
    }
    pending_device_.clear();
    if (ok) {
        if (do_start(device_id.toStdString())) {
            // Only a session that is really streaming is worth picking back
            // up later, and only the password that just worked may be kept.
            auto_device_ = device_id.toStdString();
            auto_password_ = pending_password_;
            auto_attempts_ = 0;
        }
        pending_password_.clear();
        return;
    }
    pending_password_.clear();
    if (device_id.toStdString() == auto_device_ &&
        !device_is_online(device_id.toStdString())) {
        // Retried too late in a restart: the device is simply not there, which
        // the next registration will try again. Not a wrong password.
        mlog::warn("Auto-resume of " + device_id.toStdString() +
                   " found no live session");
        return;
    }
    mlog::warn("Control authorization failed for " + device_id.toStdString());
    disarm_auto_resume();
    emit control_denied(device_id);
}

void RemoteController::ping() {
    const std::string id = controlled_device();
    if (id.empty()) {
        return;
    }
    const uint64_t now = proto::steady_us();
    if (ping_sent_us_ != 0) {
        // One Ping in flight: the answer to it is still owed.
        const int64_t age = static_cast<int64_t>(now) -
                            static_cast<int64_t>(ping_sent_us_);
        if (age < kPingTimeoutUs) {
            return;
        }
        ++ping_unanswered_;
        // Past the budget the peer is almost certainly older than this message, so
        // stop probing every second - but go on probing. A reply that was merely
        // stuck behind a busy thread must not cost the reading for the rest of the
        // session, which is what a permanent give-up did.
        if (ping_unanswered_ >= kPingGiveUp &&
            ++ping_retry_tick_ % kPingSlowRetry != 0) {
            return;
        }
    }
    ping_sent_us_ = now;
    tunnels_.send_to_device(id, proto::MessageType::Ping,
                            proto::make_ping_payload(ping_sent_us_));
}

void RemoteController::on_pong(QString device_id, quint64 t0_us, quint64 t3_us) {
    const std::string id = device_id.toStdString();
    if (id.empty() || id != controlled_device() || t0_us != ping_sent_us_) {
        return;  // the answer to a Ping already written off as lost
    }
    const int64_t rtt_us = static_cast<int64_t>(t3_us) - static_cast<int64_t>(t0_us);
    ping_sent_us_ = 0;  // whichever way this goes, the slot is free again
    if (rtt_us < 0 || rtt_us >= kPingTimeoutUs) {
        return;  // a clock moved mid-exchange; learn nothing
    }
    ping_unanswered_ = 0;
    const int rtt = static_cast<int>(rtt_us / 1000);
    // Drifted rather than replaced: a reply can be late because the thread that
    // wrote it was descheduled, and the reading must not swing on that.
    rtt_ms_ = rtt_ms_ >= 0 ? (rtt_ms_ * 3 + rtt) / 4 : rtt;
}
