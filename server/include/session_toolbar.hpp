#pragma once

#include <QWidget>

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
    void set_fps(int net_fps, int decoded_fps);
    void set_quality_index(int index);
    int quality_index() const;

signals:
    void quality_selected(int index);
    void stop_requested();
    void start_requested();
    void logon_requested();

private:
    QLabel* title_ = nullptr;
    QLabel* detail_ = nullptr;
    QLabel* state_ = nullptr;
    QComboBox* quality_ = nullptr;
    QPushButton* logon_button_ = nullptr;
    QPushButton* stop_button_ = nullptr;
    QLabel* fps_ = nullptr;
    bool streaming_ = false;
};
