#pragma once

#include <QKeySequence>
#include <QWidget>

#include <string>

#include "tunnel_manager.hpp"

class QTabWidget;
class SessionView;

// The right-hand half of the window: one closable tab per machine, plus the
// empty-state page shown while there are none. It also owns the window-level
// half of fullscreen, because only the window can resize itself.
class SessionsArea : public QWidget {
    Q_OBJECT

public:
    SessionsArea(TunnelManager& tunnels, const QKeySequence& release_key,
                 QWidget* parent = nullptr);

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
    // The window hides its roster and goes fullscreen for the current tab.
    void zoom_changed(bool on);

private:
    // Fullscreen follows whichever tab is current, so switching machines never
    // drops the operator back to a windowed view.
    void refresh_zoom();
    int tab_of(const std::string& device_id) const;
    void close_tab(int index);

    TunnelManager& tunnels_;
    QTabWidget* tabs_ = nullptr;
    QKeySequence release_key_;
    int default_quality_ = 0;
    bool zoom_wanted_ = false;
    SessionView* zoomed_ = nullptr;
};
