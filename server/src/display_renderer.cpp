#include "display_renderer.hpp"

#include <windows.h>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

namespace {

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
    {Qt::Key_Meta, VK_LWIN},
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

}  // namespace

DisplayRenderer::DisplayRenderer(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setStyleSheet("background-color: #1a1a1a;");
}

void DisplayRenderer::set_remote_size(int width, int height) {
    remote_w_ = width;
    remote_h_ = height;
}

void DisplayRenderer::set_frame(const QImage& frame) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = frame;
    }
    update();
}

void DisplayRenderer::clear_frame() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = QImage();
    }
    update();
}

QRect DisplayRenderer::remote_rect() const {
    if (remote_w_ <= 0 || remote_h_ <= 0) {
        return QRect();
    }
    QSize avail = size();
    QSize scaled = QSize(remote_w_, remote_h_).scaled(avail, Qt::KeepAspectRatio);
    int x = (avail.width() - scaled.width()) / 2;
    int y = (avail.height() - scaled.height()) / 2;
    return QRect(x, y, scaled.width(), scaled.height());
}

QPoint DisplayRenderer::map_to_remote(const QPoint& widget_pos) const {
    QRect r = remote_rect();
    if (r.width() <= 0 || r.height() <= 0 || remote_w_ <= 0 || remote_h_ <= 0) {
        return QPoint(0, 0);
    }
    int rx = (widget_pos.x() - r.x()) * remote_w_ / r.width();
    int ry = (widget_pos.y() - r.y()) * remote_h_ / r.height();
    rx = rx < 0 ? 0 : (rx > remote_w_ - 1 ? remote_w_ - 1 : rx);
    ry = ry < 0 ? 0 : (ry > remote_h_ - 1 ? remote_h_ - 1 : ry);
    return QPoint(rx, ry);
}

void DisplayRenderer::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(26, 26, 26));
    QImage frame;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frame = current_;
    }
    if (frame.isNull()) {
        painter.setPen(QColor(120, 120, 120));
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("No active session\nDouble-click a device to start"));
        return;
    }
    QRect target = remote_rect();
    if (target.isEmpty()) {
        target = rect();
    }
    painter.drawImage(target, frame);
}

void DisplayRenderer::mouseMoveEvent(QMouseEvent* event) {
    QPoint p = map_to_remote(event->pos());
    emit mouse_moved(p.x(), p.y());
}

void DisplayRenderer::mousePressEvent(QMouseEvent* event) {
    setFocus();
    QPoint p = map_to_remote(event->pos());
    emit mouse_moved(p.x(), p.y());
    int button = event->button() == Qt::RightButton ? 1
                 : event->button() == Qt::MiddleButton ? 2 : 0;
    emit mouse_button_changed(button, true);
}

void DisplayRenderer::mouseReleaseEvent(QMouseEvent* event) {
    int button = event->button() == Qt::RightButton ? 1
                 : event->button() == Qt::MiddleButton ? 2 : 0;
    emit mouse_button_changed(button, false);
}

void DisplayRenderer::wheelEvent(QWheelEvent* event) {
    emit mouse_wheelled(event->angleDelta().y());
}

void DisplayRenderer::keyPressEvent(QKeyEvent* event) {
    bool extended = false;
    WORD vk = map_to_vk(event, extended);
    if (vk != 0) {
        emit key_changed(vk, true, extended);
    }
    event->accept();
}

void DisplayRenderer::keyReleaseEvent(QKeyEvent* event) {
    bool extended = false;
    WORD vk = map_to_vk(event, extended);
    if (vk != 0) {
        emit key_changed(vk, false, extended);
    }
    event->accept();
}
