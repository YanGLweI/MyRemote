#pragma once

#include <QKeySequence>
#include <QMainWindow>
#include <QSettings>

#include <memory>

#include "config.hpp"
#include "tunnel_manager.hpp"

class DevicePanel;
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
    // The sessions area decides which tab is zoomed; the window is the only
    // thing that can hide the roster and change its own state.
    void on_zoom_changed(bool on);

private:
    // One roster record in, one row + one tab header out.
    void publish_row(const TunnelManager::DeviceInfo& info);
    void prompt_remark(const QString& device_id);
    QString remark_of(const std::string& device_id) const;
    QString display_name(const TunnelManager::DeviceInfo& info) const;

    std::unique_ptr<TunnelManager> tunnels_;
    QWidget* side_panel_ = nullptr;
    DevicePanel* device_list_ = nullptr;
    SessionsArea* sessions_ = nullptr;
    QPushButton* disconnect_button_ = nullptr;
    QPushButton* remark_button_ = nullptr;
    QKeySequence release_key_{QStringLiteral("Ctrl+Alt+Shift+R")};
    bool zoomed_ = false;
    QSettings settings_{"MyRemote", "control_server"};
};
