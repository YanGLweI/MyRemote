#pragma once

#include <QKeySequence>
#include <QWidget>

#include <memory>
#include <string>

#include "remote_controller.hpp"

class DisplayRenderer;
class InputGateway;
class SessionToolbar;
class TunnelManager;

// One remote desktop, with its own decoder thread and its own controller. The
// tab's lifetime is the session's lifetime: closing the tab stops streaming and
// releases the decoder, and nothing here depends on the roster's selection.
class SessionView : public QWidget {
    Q_OBJECT

public:
    SessionView(std::string device_id, TunnelManager& tunnels,
                int default_quality_index, const QKeySequence& release_key,
                QWidget* parent = nullptr);
    ~SessionView() override;

    const std::string& device_id() const { return device_id_; }
    // Ask the agent for a stream; prompts for the control password.
    void begin();
    // The tunnel dropped: stop decoding and clear the picture, but stay ready
    // to pick the stream back up when the device re-registers.
    void suspend();
    void set_header(const QString& device_name, const QString& detail,
                    const QString& state_text, bool live);
    // Set by the area, which owns the window-level half of fullscreen.
    void set_zoomed(bool zoomed);
    // The window owns this hotkey and may change it while the session runs.
    void set_release_key(const QKeySequence& key);
    // Lets the window remember what the operator last chose.
    int quality_index() const;

signals:
    void note(QString text);
    void zoom_requested(bool on);
    // The operator asked for the keyboard back; the tab bar is the useful place
    // for focus once the picture no longer wants keystrokes.
    void escape_released();

private:
    void refresh_buttons();

    std::string device_id_;
    TunnelManager& tunnels_;
    SessionToolbar* toolbar_ = nullptr;
    DisplayRenderer* renderer_ = nullptr;
    InputGateway* gateway_ = nullptr;
    // Owned here rather than as a Qt child so the destructor can stop the
    // decode thread before the widget it paints into is torn down.
    std::unique_ptr<RemoteController> controller_;
};
