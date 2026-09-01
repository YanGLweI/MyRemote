#pragma once

#include <QKeySequence>
#include <QSize>
#include <QVector>
#include <QWidget>
#include <string>

class QComboBox;
class QLabel;
class QPushButton;

// The chrome above one remote desktop. Everything an operator can do to a
// single machine lives here, so a session never depends on the roster being
// selected to be closed or tuned.
class SessionToolbar : public QWidget {
    Q_OBJECT

public:
    explicit SessionToolbar(QWidget* parent = nullptr);

    void set_title(const QString& device_name, const QString& detail);
    // The live/reconnecting/offline word belongs to the roster, but the session
    // has to say the same thing next to the picture it is about.
    void set_state_text(const QString& text, bool live);
    void set_streaming(bool on);
    void set_supports_logon(bool on);
    // Whether keystrokes are going to the far side. The picture cannot show it
    // and guessing is how a session starts typing into the wrong machine.
    void set_capture(bool captured);
    // The sentence explaining how to hand the keyboard back names the hotkey,
    // so it has to be rebuilt when the operator changes it.
    void set_release_key(const QKeySequence& key);
    void set_zoomed(bool zoomed);
    void set_stats(int fps, int latency_ms);
    void set_quality_index(int index);
    // Encoder mode indicator (HEVC/H.264/soft badge)
    void set_encoder_mode(const QString& mode_string);
    // The display modes the remote machine offered; an empty list (agent too
    // old to answer) leaves the picker on "--".
    void set_modes(const QVector<QSize>& modes, const QSize& current);

signals:
    void quality_selected(int index);
    void resolution_selected(int width, int height);
    void stop_requested();
    void start_requested();
    void logon_requested();
    void fullscreen_toggled(bool on);
    // Emitted when encoder mode changes (UI update hook)
    void encoder_info_updated(const std::string& info);

private:
    void refresh_capture_tip();

    QLabel* title_ = nullptr;
    QLabel* detail_ = nullptr;
    QLabel* state_ = nullptr;
    QLabel* capture_ = nullptr;
    QComboBox* quality_ = nullptr;
    QComboBox* resolution_ = nullptr;
    bool modes_known_ = false;
    QPushButton* logon_button_ = nullptr;
    QPushButton* fullscreen_button_ = nullptr;
    QPushButton* stop_button_ = nullptr;
    QLabel* stats_ = nullptr;
    bool streaming_ = false;
    QLabel* encoder_badge_ = nullptr;  // HEVC/H.264 mode indicator
    // Only ever read to word the tooltip; the gateway is what acts on it.
    QKeySequence release_key_;
};
