#pragma once

#include <QMainWindow>
#include <QSettings>

#include <memory>

#include "config.hpp"
#include "display_renderer.hpp"

class DeviceListWidget;
class QComboBox;
class QLabel;
class QPushButton;
class RemoteController;
class TunnelManager;

// The operator console: a device roster on the left, the remote desktop view on
// the right. Owns the listening TunnelManager and the one control session.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const config::ServerConfig& cfg, QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void on_device_registered(QString device_id, QString device_name, int width,
                              int height);
    void on_refresh_remark(QString device_id);
    void on_control_requested(QString device_id);

private:
    DisplayRenderer renderer_;
    std::unique_ptr<TunnelManager> tunnels_;
    std::unique_ptr<RemoteController> controller_;
    DeviceListWidget* device_list_ = nullptr;
    QPushButton* stop_button_ = nullptr;
    QPushButton* logon_button_ = nullptr;
    QPushButton* fullscreen_button_ = nullptr;
    QPushButton* disconnect_button_ = nullptr;
    QPushButton* remark_button_ = nullptr;
    QComboBox* quality_combo_ = nullptr;
    QLabel* fps_label_ = nullptr;
    QSettings remarks_{"MyRemote", "control_server"};
};
