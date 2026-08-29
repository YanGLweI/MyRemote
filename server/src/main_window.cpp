#include "main_window.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>

#include <cstdlib>
#include <string>

#include "device_list.hpp"
#include "log.hpp"
#include "remote_controller.hpp"
#include "tunnel_manager.hpp"

namespace {

// A limited agent's SendInput is dropped by UIPI on elevated windows, which
// looks exactly like a dead session; flag those devices up front.
std::string device_label(const TunnelManager::DeviceInfo& info,
                         const QString& remark) {
    std::string name = remark.isEmpty() ? info.device_name
                                        : remark.toStdString();
    if (info.flags & proto::kFlagServiceHost) {
        name += " 〔服务〕";
    }
    if (info.flags & proto::kFlagLogonScreen) {
        name += " 〔登录界面〕";
    } else if (info.elevation_known && !(info.flags & proto::kFlagConsoleOwner)) {
        name += " 〔非控制台〕";
    }
    if (info.elevation_known && !info.elevated) {
        name += " 〔受限〕";
    }
    return name;
}

}  // namespace

MainWindow::MainWindow(const config::ServerConfig& cfg, QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("MyRemote Control Center"));
    resize(1100, 700);

    tunnels_ = std::make_unique<TunnelManager>(cfg.secret_key, cfg.max_connections);
    controller_ = std::make_unique<RemoteController>(*tunnels_, renderer_);

    auto* central = new QWidget();
    setCentralWidget(central);
    auto* root_layout = new QHBoxLayout(central);

    // Left: device roster.
    auto* side_panel = new QWidget();
    auto* side_layout = new QVBoxLayout(side_panel);
    side_layout->setContentsMargins(0, 0, 0, 0);
    device_list_ = new DeviceListWidget();
    quality_combo_ = new QComboBox();
    quality_combo_->addItem(QStringLiteral("Smooth (30fps/1.5M/1280)"));
    quality_combo_->addItem(QStringLiteral("Balanced (30fps/2M/1920)"));
    quality_combo_->addItem(QStringLiteral("Sharp (60fps/6M/device cap)"));
    quality_combo_->setCurrentIndex(1);
    fullscreen_button_ = new QPushButton(QStringLiteral("Fullscreen"));
    remark_button_ = new QPushButton(QStringLiteral("Set Remark"));
    remark_button_->setEnabled(false);
    stop_button_ = new QPushButton(QStringLiteral("Stop Control"));
    stop_button_->setEnabled(false);
    logon_button_ = new QPushButton(QStringLiteral("返回登录界面"));
    logon_button_->setEnabled(false);
    logon_button_->setToolTip(QStringLiteral(
        "锁定工作站，让对端回到登录界面后可以远程输入密码登录。\n"
        "Ctrl+Alt+Del 无法被注入：若策略要求按 Ctrl+Alt+Del 才开始登录，\n"
        "请将该机器的 DisableCAD 设为 1。"));
    side_layout->addWidget(device_list_, 1);
    side_layout->addWidget(quality_combo_);
    side_layout->addWidget(fullscreen_button_);
    side_layout->addWidget(remark_button_);
    side_layout->addWidget(logon_button_);
    side_layout->addWidget(stop_button_);
    disconnect_button_ = new QPushButton(QStringLiteral("Disconnect Selected"));
    side_layout->addWidget(disconnect_button_);
    side_panel->setFixedWidth(340);
    root_layout->addWidget(side_panel);

    // Right: remote desktop view with FPS overlay.
    auto* right_panel = new QWidget();
    auto* right_layout = new QVBoxLayout(right_panel);
    right_layout->setContentsMargins(0, 0, 0, 0);
    fps_label_ = new QLabel(QStringLiteral("FPS: --"));
    fps_label_->setAlignment(Qt::AlignRight);
    fps_label_->setStyleSheet("color: #2E8B57; font-weight: bold;");
    right_layout->addWidget(fps_label_);
    right_layout->addWidget(&renderer_, 1);
    root_layout->addWidget(right_panel, 1);

    statusBar()->showMessage(QStringLiteral("Ready"), 0);

    connect(device_list_, &DeviceListWidget::remote_control_requested, this,
            &MainWindow::on_control_requested);
    connect(device_list_, &DeviceListWidget::selection_changed, this,
            [this](QString id) {
                remark_button_->setEnabled(!id.isEmpty());
            });
    connect(stop_button_, &QPushButton::clicked, this,
            [this]() { controller_->stop_control(); });
    connect(quality_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
                static const int kFps[] = {30, 30, 60};
                static const int kBr[] = {1500, 2048, 6000};
                static const int kW[] = {1280, 1920, 0};
                if (idx >= 0 && idx < 3) {
                    controller_->apply_quality(
                        static_cast<uint8_t>(kFps[idx]),
                        static_cast<uint16_t>(kBr[idx]),
                        static_cast<uint16_t>(kW[idx]));
                }
            });
    connect(fullscreen_button_, &QPushButton::clicked, this, [this]() {
        if (isFullScreen()) {
            showNormal();
        } else {
            showFullScreen();
        }
    });
    connect(disconnect_button_, &QPushButton::clicked, this, [this]() {
        for (const auto& id : device_list_->selected_device_ids()) {
            if (controller_->is_controlling() &&
                controller_->controlled_device() == id) {
                controller_->stop_control();
            }
            tunnels_->disconnect_device(id);
        }
    });
    connect(remark_button_, &QPushButton::clicked, this, [this]() {
        auto id = device_list_->selected_device_id();
        if (!id.has_value()) return;
        bool ok = false;
        QString cur = remarks_.value(QString::fromStdString(*id)).toString();
        QString text = QInputDialog::getText(this, QStringLiteral("Set Remark"),
                                             QStringLiteral("Remark for this device:"),
                                             QLineEdit::Normal, cur, &ok);
        if (ok) {
            remarks_.setValue(QString::fromStdString(*id), text);
            on_refresh_remark(QString::fromStdString(*id));
        }
    });
    connect(tunnels_.get(), &TunnelManager::device_registered, this,
            &MainWindow::on_device_registered);
    connect(tunnels_.get(), &TunnelManager::device_state_changed, this,
            [this](QString device_id, int) {
                TunnelManager::DeviceInfo info;
                if (!tunnels_->roster_for(device_id.toStdString(), &info)) {
                    return;
                }
                upsert_row(info);
                if (info.state != DeviceState::Live &&
                    controller_->is_controlling() &&
                    controller_->controlled_device() == device_id.toStdString()) {
                    // A service-managed host comes back in about two seconds,
                    // so keep the session armed instead of ending it.
                    controller_->suspend_control();
                }
            });
    connect(controller_.get(), &RemoteController::status_note, this,
            [this](QString text) { statusBar()->showMessage(text, 8000); });
    connect(controller_.get(), &RemoteController::control_started, this,
            [this](QString device_id) {
                stop_button_->setEnabled(true);
                logon_button_->setEnabled(controller_->controlled_supports_logon());
                statusBar()->showMessage(
                    QStringLiteral("Controlling %1").arg(device_id), 0);
            });
    connect(controller_.get(), &RemoteController::control_stopped, this,
            [this]() {
                stop_button_->setEnabled(false);
                logon_button_->setEnabled(false);
                fps_label_->setText(QStringLiteral("FPS: --"));
                statusBar()->showMessage(QStringLiteral("Ready"), 0);
            });
    connect(logon_button_, &QPushButton::clicked, this,
            [this]() { controller_->lock_workstation(); });
    connect(tunnels_.get(), &TunnelManager::video_frame_received,
            controller_.get(), &RemoteController::on_video_frame,
            Qt::DirectConnection);
    connect(tunnels_.get(), &TunnelManager::auth_result, controller_.get(),
            &RemoteController::on_auth_result);
    connect(controller_.get(), &RemoteController::control_denied, this,
            [this](QString id) {
                QMessageBox::warning(this, QStringLiteral("MyRemote"),
                    QStringLiteral("Wrong control password for %1").arg(id));
            });
    connect(&renderer_, &DisplayRenderer::mouse_moved, controller_.get(),
            &RemoteController::on_mouse_moved);
    connect(&renderer_, &DisplayRenderer::mouse_button_changed, controller_.get(),
            &RemoteController::on_mouse_button);
    connect(&renderer_, &DisplayRenderer::mouse_wheelled, controller_.get(),
            &RemoteController::on_mouse_wheel);
    connect(&renderer_, &DisplayRenderer::key_changed, controller_.get(),
            &RemoteController::on_key);
    connect(controller_.get(), &RemoteController::fps_updated, this,
            [this](int net_fps, int decoded_fps) {
                if (controller_->is_controlling()) {
                    fps_label_->setText(
                        QStringLiteral("NET %1 | DEC %2 fps")
                            .arg(net_fps)
                            .arg(decoded_fps));
                }
            });

    if (!tunnels_->start(cfg.bind_address, cfg.listening_port)) {
        QMessageBox::critical(this, QStringLiteral("MyRemote"),
                              QStringLiteral("Failed to listen on port %1")
                                  .arg(cfg.listening_port));
        std::exit(1);
    }
}

MainWindow::~MainWindow() {
    if (controller_) {
        controller_->stop_control();
    }
    if (tunnels_) {
        tunnels_->stop();
    }
}

void MainWindow::upsert_row(const TunnelManager::DeviceInfo& stored) {
    TunnelManager::DeviceInfo info = stored;
    QString remark = remarks_.value(QString::fromStdString(info.device_id)).toString();
    info.device_name = device_label(info, remark);
    device_list_->upsert_device(info);
}

void MainWindow::on_device_registered(QString device_id, QString, int, int) {
    TunnelManager::DeviceInfo info;
    if (!tunnels_->roster_for(device_id.toStdString(), &info)) {
        return;  // a rejected or not-yet-installed registration
    }
    upsert_row(info);
    // Capabilities move at runtime (logon screen, console owner).
    if (controller_->is_controlling()) {
        logon_button_->setEnabled(controller_->controlled_supports_logon());
    }
}

void MainWindow::on_refresh_remark(QString device_id) {
    TunnelManager::DeviceInfo info;
    if (tunnels_->roster_for(device_id.toStdString(), &info)) {
        upsert_row(info);
    }
}

void MainWindow::on_control_requested(QString device_id) {
    if (controller_->is_controlling()) {
        if (controller_->controlled_device() == device_id.toStdString()) {
            return;
        }
        QMessageBox::information(
            this, QStringLiteral("MyRemote"),
            QStringLiteral("Stop the current session before switching devices."));
        return;
    }
    bool ok = false;
    QString password = QInputDialog::getText(
        this, QStringLiteral("Control authorization"),
        QStringLiteral("Enter the device control password (leave empty if none):"),
        QLineEdit::Password, QString(), &ok);
    if (!ok) {
        return;
    }
    controller_->request_control(device_id.toStdString(), password.toStdString());
}
