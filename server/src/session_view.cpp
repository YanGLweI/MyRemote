#include "session_view.hpp"

#include <QInputDialog>
#include <QLineEdit>
#include <QVBoxLayout>

#include "display_renderer.hpp"
#include "input_gateway.hpp"
#include "log.hpp"
#include "session_toolbar.hpp"
#include "tunnel_manager.hpp"

SessionView::SessionView(std::string device_id, TunnelManager& tunnels,
                         int default_quality_index, const QKeySequence& release_key,
                         QWidget* parent)
    : QWidget(parent), device_id_(device_id), tunnels_(tunnels) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    toolbar_ = new SessionToolbar(this);
    renderer_ = new DisplayRenderer(this);
    layout->addWidget(toolbar_);
    layout->addWidget(renderer_, 1);

    gateway_ = new InputGateway(renderer_, this);
    renderer_->installEventFilter(gateway_);

    controller_ = std::make_unique<RemoteController>(tunnels_, *renderer_);
    toolbar_->set_quality_index(default_quality_index);
    // The first StartStream must carry the preset the picker already shows,
    // not the controller's private defaults.
    if (default_quality_index >= 0 && default_quality_index < kQualityPresetCount) {
        const QualityPreset& p = kQualityPresets[default_quality_index];
        controller_->apply_quality(p.fps, p.bitrate_kbps, p.max_encode_width);
    }
    toolbar_->set_title(QString::fromStdString(device_id), QString());
    // After the title: the same hotkey has to reach the thing that acts on it
    // and the sentence that names it.
    set_release_key(release_key);

    // Video frames arrive on the tunnel thread and may only be handed to the
    // decode queue, so this connection stays direct by design.
    connect(&tunnels_, &TunnelManager::video_frame_received, controller_.get(),
            &RemoteController::on_video_frame, Qt::DirectConnection);
    connect(&tunnels_, &TunnelManager::auth_result, controller_.get(),
            &RemoteController::on_auth_result);
    connect(renderer_, &DisplayRenderer::mouse_moved, controller_.get(),
            &RemoteController::on_mouse_moved);
    connect(renderer_, &DisplayRenderer::mouse_button_changed, controller_.get(),
            &RemoteController::on_mouse_button);
    connect(renderer_, &DisplayRenderer::mouse_wheelled, controller_.get(),
            &RemoteController::on_mouse_wheel);
    connect(gateway_, &InputGateway::key_changed, controller_.get(),
            &RemoteController::on_key);
    connect(gateway_, &InputGateway::capture_changed, toolbar_,
            &SessionToolbar::set_capture);
    connect(gateway_, &InputGateway::escape_released, this,
            &SessionView::escape_released);
    // The frame rate is what reached the screen; the latency is how old those
    // pictures were, and is -1 until this device's clock has been tied to ours.
    connect(controller_.get(), &RemoteController::stats_updated, toolbar_,
            &SessionToolbar::set_stats);
    connect(controller_.get(), &RemoteController::control_started, this,
            [this](QString) { refresh_buttons(); });
    connect(controller_.get(), &RemoteController::control_stopped, this,
            [this] { refresh_buttons(); });
    connect(controller_.get(), &RemoteController::status_note, this,
            &SessionView::note);
    connect(controller_.get(), &RemoteController::control_denied, this,
            [this](QString id) {
                emit note(QStringLiteral("%1 的控制密码不正确").arg(id));
            });

    connect(toolbar_, &SessionToolbar::quality_selected, this, [this](int idx) {
        if (idx < 0 || idx >= kQualityPresetCount) {
            return;
        }
        const QualityPreset& p = kQualityPresets[idx];
        controller_->apply_quality(p.fps, p.bitrate_kbps, p.max_encode_width);
        emit quality_changed(idx);
    });
    connect(controller_.get(), &RemoteController::display_modes_ready, toolbar_,
            &SessionToolbar::set_modes);
    connect(&tunnels_, &TunnelManager::codec_capabilities, controller_.get(),
            &RemoteController::on_codec_capabilities);
    connect(controller_.get(), &RemoteController::encoder_mode_changed, toolbar_,
            &SessionToolbar::set_encoder_mode);
    connect(toolbar_, &SessionToolbar::resolution_selected, this,
            [this](int w, int h) { controller_->set_display_mode(w, h); });
    connect(toolbar_, &SessionToolbar::stop_requested, this, [this] {
        // Flushes any key still held down while the tunnel is still open.
        gateway_->set_captured(false);
        controller_->stop_control();
    });
    connect(toolbar_, &SessionToolbar::start_requested, this,
            [this] { begin(); });
    connect(toolbar_, &SessionToolbar::logon_requested, this, [this] {
        controller_->lock_workstation();
    });
    connect(toolbar_, &SessionToolbar::fullscreen_toggled, this,
            &SessionView::zoom_requested);
}

SessionView::~SessionView() {
    // Hand back the keyboard first so any key still physically held is released
    // on the far side while its session is still open, then end the session so
    // the decode thread is joined while the renderer it paints into is alive,
    // then drop the controller before Qt starts destroying child widgets.
    gateway_->set_captured(false);
    controller_->stop_control();
    controller_.reset();
}

void SessionView::begin() {
    if (controller_->is_controlling()) {
        return;
    }
    bool ok = false;
    QString password = QInputDialog::getText(
        this, QStringLiteral("控制授权"),
        QStringLiteral("输入该设备的控制密码（未设置则留空）："), QLineEdit::Password,
        QString(), &ok);
    if (!ok) {
        return;
    }
    controller_->request_control(device_id_, password.toStdString());
}

void SessionView::suspend() {
    controller_->suspend_control();
    refresh_buttons();
}

void SessionView::set_header(const QString& device_name, const QString& detail,
                             const QString& state_text, bool live) {
    toolbar_->set_title(device_name, detail);
    toolbar_->set_state_text(state_text, live);
    refresh_buttons();
}

void SessionView::set_zoomed(bool zoomed) { toolbar_->set_zoomed(zoomed); }

void SessionView::set_release_key(const QKeySequence& key) {
    gateway_->set_release_key(key);
    toolbar_->set_release_key(key);
}

void SessionView::refresh_buttons() {
    const bool streaming = controller_->is_controlling();
    if (!streaming) {
        gateway_->set_captured(false);
    }
    toolbar_->set_streaming(streaming);
    toolbar_->set_supports_logon(streaming &&
                                 controller_->controlled_supports_logon());
}
