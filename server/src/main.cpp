// MyRemote Control Center (控制端)
// Passive side only: lists clients that connected out to us, and drives
// remote sessions over those client-initiated tunnels.

#include <windows.h>

#include <QApplication>
#include <QHBoxLayout>
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
        stop_button_ = new QPushButton(QStringLiteral("Stop Control"));
        stop_button_->setEnabled(false);
        side_layout->addWidget(device_list_, 1);
        side_layout->addWidget(stop_button_);
        side_panel->setFixedWidth(340);
        root_layout->addWidget(side_panel);

        // Right: remote desktop view.
        root_layout->addWidget(&renderer_, 1);

        statusBar()->showMessage(QStringLiteral("Ready"), 0);

        connect(device_list_, &DeviceListWidget::remote_control_requested, this,
                &MainWindow::on_control_requested);
        connect(stop_button_, &QPushButton::clicked, this,
                [this]() { controller_->stop_control(); });
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
                    statusBar()->showMessage(QStringLiteral("Ready"), 0);
                });

        if (!tunnels_->start(cfg.bind_address, cfg.listening_port)) {
            QMessageBox::critical(this, QStringLiteral("MyRemote"),
                                  QStringLiteral("Failed to listen on port %1")
                                      .arg(cfg.listening_port));
            std::exit(1);
        }
    }

    ~MainWindow() override {
        if (tunnels_) {
            tunnels_->stop();
        }
    }

private slots:
    void on_device_registered(QString device_id, QString, int, int) {
        for (const auto& info : tunnels_->online_devices()) {
            if (QString::fromStdString(info.device_id) == device_id) {
                device_list_->upsert_device(info.device_id, info.device_name,
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
        if (!controller_->start_control(device_id.toStdString())) {
            QMessageBox::warning(this, QStringLiteral("MyRemote"),
                                 QStringLiteral("Failed to start control session."));
        }
    }

private:
    std::unique_ptr<TunnelManager> tunnels_;
    std::unique_ptr<RemoteController> controller_;
    DeviceListWidget* device_list_ = nullptr;
    DisplayRenderer renderer_;
    QPushButton* stop_button_ = nullptr;
};

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    QApplication app(__argc, __argv);
    QApplication::setApplicationName(QStringLiteral("MyRemote Control Center"));

    std::string dir = exe_dir();
    mlog::init(dir + "/" + "control_server.log");
    mlog::info("Control center starting");

    config::ServerConfig cfg =
        config::ServerConfig::load(dir + "\server_config.json");

    MainWindow window(cfg);
    window.show();
    return app.exec();
}

#include "main.moc"
