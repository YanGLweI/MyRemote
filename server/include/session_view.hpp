#pragma once

#include <QWidget>

#include <memory>
#include <string>

#include "remote_controller.hpp"

class DisplayRenderer;
class SessionToolbar;
class TunnelManager;

// One remote desktop, with its own decoder thread and its own controller. The
// tab's lifetime is the session's lifetime: closing the tab stops streaming and
// releases the decoder, and nothing here depends on the roster's selection.
class SessionView : public QWidget {
    Q_OBJECT

public:
    SessionView(std::string device_id, TunnelManager& tunnels,
                int default_quality_index, QWidget* parent = nullptr);
    ~SessionView() override;

    const std::string& device_id() const { return device_id_; }
    // Ask the agent for a stream; prompts for the control password.
    void begin();
    // The tunnel dropped: stop decoding and clear the picture, but stay ready
    // to pick the stream back up when the device re-registers.
    void suspend();
    void set_header(const QString& device_name, const QString& detail,
                    const QString& state_text, bool live);
    // Lets the window remember what the operator last chose.
    int quality_index() const;
    bool streaming() const;
    bool supports_logon() const;

signals:
    void closed(QString device_id);
    void note(QString text);

private:
    void refresh_buttons();

    std::string device_id_;
    TunnelManager& tunnels_;
    SessionToolbar* toolbar_ = nullptr;
    DisplayRenderer* renderer_ = nullptr;
    // Owned here rather than as a Qt child so the destructor can stop the
    // decode thread before the widget it paints into is torn down.
    std::unique_ptr<RemoteController> controller_;
};
