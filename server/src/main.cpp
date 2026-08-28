// MyRemote Control Center (控制端)
// Passive side only: lists clients that connected out to us, and drives
// remote sessions over those client-initiated tunnels.

#include <windows.h>

#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QSettings>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>

#include <memory>
#include <string>

#include "config.hpp"
#include "device_list.hpp"
#include "display_renderer.hpp"
#include "log.hpp"
#include "remote_controller.hpp"
#include "tunnel_manager.hpp"

namespace {

std::string exe_dir() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    size_t pos = s.find_last_of("\\/");
    return pos == std::string::npos ? "." : s.substr(0, pos);
}

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(const config::ServerConfig& cfg) {
        setWindowTitle(QStringLiteral("MyRemote Control Center"));
        resize(1100, 700);

        tunnels_ = std::make_unique<TunnelManager>(cfg.secret_key,
                                                   cfg.max_connections);
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
        quality_combo_->addItem(QStringLiteral("Low latency (30fps/1.5M)"));
        quality_combo_->addItem(QStringLiteral("Balanced (30fps/2M)"));
        quality_combo_->addItem(QStringLiteral("High quality (60fps/6M)"));
        quality_combo_->setCurrentIndex(1);
        fullscreen_button_ = new QPushButton(QStringLiteral("Fullscreen"));
        remark_button_ = new QPushButton(QStringLiteral("Set Remark"));
        remark_button_->setEnabled(false);
        stop_button_ = new QPushButton(QStringLiteral("Stop Control"));
        stop_button_->setEnabled(false);
        side_layout->addWidget(device_list_, 1);
        side_layout->addWidget(quality_combo_);
        side_layout->addWidget(fullscreen_button_);
        side_layout->addWidget(remark_button_);
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
                    if (idx >= 0 && idx < 3) {
                        controller_->apply_quality(static_cast<uint8_t>(kFps[idx]),
                                                   static_cast<uint16_t>(kBr[idx]));
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
        connect(tunnels_.get(), &TunnelManager::device_unregistered, this,
                [this](QString device_id) {
                    device_list_->remove_device(device_id.toStdString());
                    if (controller_->is_controlling() &&
                        controller_->controlled_device() == device_id.toStdString()) {
                        controller_->stop_control();
                    }
                });
        connect(controller_.get(), &RemoteController::control_started, this,
                [this](QString device_id) {
                    stop_button_->setEnabled(true);
                    statusBar()->showMessage(
                        QStringLiteral("Controlling %1").arg(device_id), 0);
                });
        connect(controller_.get(), &RemoteController::control_stopped, this,
                [this]() {
                    stop_button_->setEnabled(false);
                    fps_label_->setText(QStringLiteral("FPS: --"));
                    statusBar()->showMessage(QStringLiteral("Ready"), 0);
                });
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

    ~MainWindow() override {
        if (controller_) {
            controller_->stop_control();
        }
        if (tunnels_) {
            tunnels_->stop();
        }
    }

private slots:
    void on_device_registered(QString device_id, QString, int, int) {
        for (const auto& info : tunnels_->online_devices()) {
            if (QString::fromStdString(info.device_id) == device_id) {
                QString remark = remarks_.value(QString::fromStdString(info.device_id)).toString();
                std::string display = remark.isEmpty() ? info.device_name
                                                       : remark.toStdString();
                device_list_->upsert_device(info.device_id, display,
                                            info.screen_width, info.screen_height,
                                            info.peer_ip, info.connect_time);
                return;
            }
        }
    }

    void on_refresh_remark(QString device_id) {
        for (const auto& info : tunnels_->online_devices()) {
            if (QString::fromStdString(info.device_id) == device_id) {
                QString remark = remarks_.value(device_id).toString();
                std::string display = remark.isEmpty() ? info.device_name : remark.toStdString();
                device_list_->upsert_device(info.device_id, display,
                                            info.screen_width, info.screen_height,
                                            info.peer_ip, info.connect_time);
                return;
            }
        }
    }

    void on_control_requested(QString device_id) {
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

private:
    DisplayRenderer renderer_;
    std::unique_ptr<TunnelManager> tunnels_;
    std::unique_ptr<RemoteController> controller_;
    DeviceListWidget* device_list_ = nullptr;
    QPushButton* stop_button_ = nullptr;
    QPushButton* fullscreen_button_ = nullptr;
    QPushButton* disconnect_button_ = nullptr;
    QPushButton* remark_button_ = nullptr;
    QComboBox* quality_combo_ = nullptr;
    QLabel* fps_label_ = nullptr;
    QSettings remarks_{"MyRemote", "control_server"};
};

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    QApplication app(__argc, __argv);
    QApplication::setApplicationName(QStringLiteral("MyRemote Control Center"));

    std::string dir = exe_dir();
    mlog::init(dir + "/" + "control_server.log");
    mlog::info("Control center starting");

    config::ServerConfig cfg =
        config::ServerConfig::load(dir + "/server_config.json");

    MainWindow window(cfg);
    window.show();
    return app.exec();
}

#include "main.moc"
