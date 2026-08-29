#include "input_gateway.hpp"

#include <windows.h>

#include <QEvent>
#include <QKeyEvent>
#include <QTimer>

#include "display_renderer.hpp"

namespace {

constexpr int kEscWindowMs = 600;

struct QtVkMap {
    int qt_key;
    WORD vk;
};

// Used only when the platform event carries no usable native VK (IME keys,
// synthetic events). Letters/digits match ASCII already, so they are omitted.
constexpr QtVkMap kQtToVk[] = {
    {Qt::Key_Escape, VK_ESCAPE},
    {Qt::Key_Tab, VK_TAB},
    {Qt::Key_Backtab, VK_TAB},
    {Qt::Key_Backspace, VK_BACK},
    {Qt::Key_Return, VK_RETURN},
    {Qt::Key_Enter, VK_RETURN},  // numpad enter
    {Qt::Key_Insert, VK_INSERT},
    {Qt::Key_Delete, VK_DELETE},
    {Qt::Key_Pause, VK_PAUSE},
    {Qt::Key_Print, VK_SNAPSHOT},
    {Qt::Key_Home, VK_HOME},
    {Qt::Key_End, VK_END},
    {Qt::Key_Left, VK_LEFT},
    {Qt::Key_Up, VK_UP},
    {Qt::Key_Right, VK_RIGHT},
    {Qt::Key_Down, VK_DOWN},
    {Qt::Key_PageUp, VK_PRIOR},
    {Qt::Key_PageDown, VK_NEXT},
    {Qt::Key_Shift, VK_SHIFT},
    {Qt::Key_Control, VK_CONTROL},
    {Qt::Key_Alt, VK_MENU},
    {Qt::Key_AltGr, VK_RMENU},
    {Qt::Key_CapsLock, VK_CAPITAL},
    {Qt::Key_NumLock, VK_NUMLOCK},
    {Qt::Key_ScrollLock, VK_SCROLL},
    {Qt::Key_Clear, VK_CLEAR},
    {Qt::Key_Super_L, VK_LWIN},
    {Qt::Key_Super_R, VK_RWIN},
    {Qt::Key_Menu, VK_APPS},
    {Qt::Key_Help, VK_HELP},
};

bool is_extended_vk(WORD vk) {
    switch (vk) {
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_UP:
        case VK_RIGHT:
        case VK_DOWN:
        case VK_NUMLOCK:
        case VK_SNAPSHOT:
        case VK_RCONTROL:   // 0xA3
        case VK_RMENU:      // 0xA5
        case VK_LWIN:
        case VK_RWIN:
        case VK_DIVIDE:
        case VK_CANCEL:
        case VK_PAUSE:
            return true;
        default:
            return false;
    }
}

// Resolves the physical Win32 virtual key for a Qt key event.
WORD map_to_vk(const QKeyEvent* event, bool& extended) {
    WORD vk = 0;
    const quint32 native = event->nativeVirtualKey();
    if (native != 0 && native != VK_PROCESSKEY && native <= 0xFE) {
        vk = static_cast<WORD>(native);
    }
    if (vk == 0) {
        const int key = event->key();
        if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
            vk = static_cast<WORD>(VK_F1 + (key - Qt::Key_F1));
        }
        for (const auto& m : kQtToVk) {
            if (m.qt_key == key) {
                vk = m.vk;
                break;
            }
        }
        if (vk == 0 && key > 0 && key <= 0xFF) {
            // Latin-1 printable: base VK; the physical Shift/Ctrl presses are
            // forwarded separately, so the remote produces the right glyph.
            const SHORT scan = VkKeyScanW(static_cast<WCHAR>(key));
            if (scan != -1) {
                vk = static_cast<WORD>(LOBYTE(scan));
            }
        }
    }
    extended = vk != 0 && is_extended_vk(vk);
    return vk;
}

// The key plus its modifiers as a sequence, so a configured hotkey can be
// compared by value: Qt 6.7 matches sequences, not key events.
QKeySequence combination(const QKeyEvent* event) {
    return QKeySequence(
        QKeyCombination(event->modifiers(), static_cast<Qt::Key>(event->key())));
}

}  // namespace

InputGateway::InputGateway(DisplayRenderer* view, QObject* parent)
    : QObject(parent), view_(view) {
    esc_timer_ = new QTimer(this);
    esc_timer_->setSingleShot(true);
    esc_timer_->setInterval(kEscWindowMs);
    connect(esc_timer_, &QTimer::timeout, this, &InputGateway::flush_pending_esc);
}

void InputGateway::set_captured(bool captured) {
    if (captured_ == captured) {
        return;
    }
    captured_ = captured;
    if (!captured_) {
        drop_pending_esc();
        release_held_keys();
    }
    emit capture_changed(captured_);
}

bool InputGateway::eventFilter(QObject* watched, QEvent* event) {
    if (watched != view_) {
        return false;
    }
    switch (event->type()) {
        case QEvent::MouseButtonPress:
            // The click itself still goes to the far side; this only decides
            // that the keyboard follows it.
            set_captured(true);
            return false;
        case QEvent::FocusOut:
        case QEvent::WindowDeactivate:
            // Key releases happening elsewhere never reach us, so letting the
            // remote keep held keys would be a stuck-key guarantee.
            set_captured(false);
            return false;
        case QEvent::KeyPress:
            return handle_key(static_cast<QKeyEvent*>(event), true);
        case QEvent::KeyRelease:
            return handle_key(static_cast<QKeyEvent*>(event), false);
        default:
            return false;
    }
}

bool InputGateway::stays_local(const QKeyEvent* event) {
    const int key = event->key();
    if (key == Qt::Key_Meta || key == Qt::Key_Super_L || key == Qt::Key_Super_R) {
        return true;
    }
    // Alt+F4 closes this window and Alt+Esc switches away from it; either one
    // would be lost if the far side swallowed it first.
    return (event->modifiers() & Qt::AltModifier) &&
           (key == Qt::Key_F4 || key == Qt::Key_Escape);
}

bool InputGateway::handle_key(QKeyEvent* event, bool pressed) {
    if (!captured_) {
        return false;
    }
    if (stays_local(event)) {
        release_held_keys();
        return false;
    }
    if (pressed && !event->isAutoRepeat() && !release_key_.isEmpty() &&
        combination(event) == release_key_) {
        set_captured(false);
        emit escape_released();
        return true;
    }

    bool extended = false;
    const WORD vk = map_to_vk(event, extended);
    if (vk == 0) {
        // Nothing mappable, and the view has focus: passing it on would let a
        // stray Space or Tab drive the local widgets.
        return true;
    }

    if (vk == VK_ESCAPE) {
        if (pressed && !event->isAutoRepeat()) {
            if (esc_pending_) {
                // The buffered first Esc is abandoned too: that pair meant
                // "release", not "close this menu".
                drop_pending_esc();
                set_captured(false);
                emit escape_released();
            } else {
                esc_pending_ = true;
                pending_vk_ = vk;
                pending_extended_ = extended;
                esc_timer_->start();
                view_->set_hint(QStringLiteral("再按一次 Esc 释放键盘"));
            }
        }
        return true;
    }

    if (esc_pending_) {
        flush_pending_esc();  // the far side sees the keys in the order typed
    }

    if (pressed) {
        if (event->isAutoRepeat() && held_.contains(vk)) {
            return true;  // the remote auto-repeats a held key by itself
        }
        held_.insert(vk, extended);
    } else if (!held_.remove(vk)) {
        return true;  // its press happened before this view had the keyboard
    }
    emit key_changed(vk, pressed, extended);
    return true;
}

void InputGateway::release_held_keys() {
    for (auto it = held_.cbegin(); it != held_.cend(); ++it) {
        emit key_changed(it.key(), false, it.value());
    }
    held_.clear();
}

void InputGateway::flush_pending_esc() {
    esc_timer_->stop();
    if (!esc_pending_) {
        return;
    }
    esc_pending_ = false;
    view_->set_hint(QString());
    emit key_changed(pending_vk_, true, pending_extended_);
    emit key_changed(pending_vk_, false, pending_extended_);
}

void InputGateway::drop_pending_esc() {
    esc_timer_->stop();
    esc_pending_ = false;
    view_->set_hint(QString());
}
