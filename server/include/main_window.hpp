#pragma once

#include <QMainWindow>
#include <QSettings>

#include <memory>

#include "config.hpp"
#include "tunnel_manager.hpp"

class DeviceListWidget;
class QPushButton;
class SessionsArea;

// The operator console: a device roster on the left, one closable tab per
// remote machine on the right. Owns the listening TunnelManager; every session
// object lives under the tab that shows it.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const config::ServerConfig& cfg, QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void on_device_registered(QString device_id, QString device_name, int width,
                              int height);
    void on_device_state_changed(QString device_id, int state);
    void on_refresh_remark(QString device_id);
    void on_control_requested(QString device_id);

private:
    // One roster record in, one row + one tab header out: the remark always
    // wins over the hostname, and the state drives marker and colour.
    void publish_row(const TunnelManager::DeviceInfo& info);
    QString display_name(const TunnelManager::DeviceInfo& info) const;

    std::unique_ptr<TunnelManager> tunnels_;
    DeviceListWidget* device_list_ = nullptr;
    SessionsArea* sessions_ = nullptr;
    QPushButton* disconnect_button_ = nullptr;
    QPushButton* remark_button_ = nullptr;
    QSettings remarks_{"MyRemote", "control_server"};
};
