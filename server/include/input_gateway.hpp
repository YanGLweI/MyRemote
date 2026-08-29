#pragma once

#include <QKeySequence>
#include <QObject>

#include <QHash>

class DisplayRenderer;
class QKeyEvent;
class QTimer;

// Decides, key by key, whether the operator's keyboard belongs to this window
// or to the machine on the screen. Installed as an event filter on the display,
// so a key it does not claim falls back into Qt's normal dispatch instead of
// disappearing.
//
// Two states. Passive forwards the mouse only. A click on the picture captures
// the keyboard; a doubled Esc, clicking outside the view, or leaving the
// application gives it back.
class InputGateway : public QObject {
    Q_OBJECT

public:
    explicit InputGateway(DisplayRenderer* view, QObject* parent = nullptr);

    bool captured() const { return captured_; }
    void set_captured(bool captured);
    // Escape hatch for layouts where a doubled Esc is awkward.
    void set_release_key(const QKeySequence& keys) { release_key_ = keys; }

signals:
    void key_changed(int vk, bool pressed, bool extended);
    void capture_changed(bool captured);
    // Only the deliberate releases: losing focus also hands the keyboard back,
    // and following that with a focus change would fight the operator.
    void escape_released();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool handle_key(QKeyEvent* event, bool pressed);
    // Alt+F4, Win and Alt+Esc are the operator's, never the far side's.
    static bool stays_local(const QKeyEvent* event);
    void release_held_keys();
    void flush_pending_esc();
    void drop_pending_esc();

    DisplayRenderer* view_;
    QTimer* esc_timer_ = nullptr;
    QKeySequence release_key_{QStringLiteral("Ctrl+Alt+Shift+R")};
    bool captured_ = false;
    // Physical keys held down on the far side, so dropping out of Captured
    // mid-keystroke cannot leave one stuck.
    QHash<int, bool> held_;
    // The first Esc is buffered rather than dropped: on its own it still has to
    // close menus remotely, just as soon as it is clear it was not a release.
    bool esc_pending_ = false;
    int pending_vk_ = 0;
    bool pending_extended_ = false;
};
