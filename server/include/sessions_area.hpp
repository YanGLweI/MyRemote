#pragma once

#include <QWidget>

#include <string>

#include "tunnel_manager.hpp"

class QTabWidget;
class SessionsArea : public QWidget {
    Q_OBJECT

public:
    explicit SessionsArea(TunnelManager& tunnels, QWidget* parent = nullptr);

    // Creates the tab if needed and always brings it forward.
    void open_session(const TunnelManager::DeviceInfo& info,
                      const QString& display_name);
    // Keeps an open tab's header honest while its tunnel changes state.
    void update_session(const TunnelManager::DeviceInfo& info,
                        const QString& display_name);
    // Tunnel gone but the tab stays: stop decoding, keep auto-resume armed.
    void suspend_session(const std::string& device_id);
    int session_count() const;
    void close_all();

signals:
    void note(QString text);

private:
    int tab_of(const std::string& device_id) const;

    TunnelManager& tunnels_;
    QTabWidget* tabs_ = nullptr;
    int default_quality_ = 0;
};
