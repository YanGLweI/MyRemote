#include "display_renderer.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include "theme.hpp"

DisplayRenderer::DisplayRenderer(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

void DisplayRenderer::set_remote_size(int width, int height) {
    remote_w_ = width;
    remote_h_ = height;
}

void DisplayRenderer::set_frame(QImage frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = std::move(frame);
}

void DisplayRenderer::repaint_frame() {
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
    const theme::Palette& c = theme::colors();
    painter.fillRect(rect(), c.canvas);
    QImage frame;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frame = current_;
    }
    if (frame.isNull()) {
        painter.setPen(c.stale);
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("等待远程画面…"));
    } else {
        QRect target = remote_rect();
        if (target.isEmpty()) {
            // The desktop-size event can trail the first frames by seconds. Until
            // it lands, letterbox on the coded size: filling the widget would
            // stretch the picture to whatever shape the pane happens to have.
            QSize fit = frame.size().scaled(size(), Qt::KeepAspectRatio);
            target = QRect(QPoint(0, 0), fit);
            target.moveCenter(rect().center());
        }
        painter.drawImage(target, frame);
    }
    if (hint_.isEmpty()) {
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing);
    const QFontMetrics fm(painter.font());
    const int chip_h = fm.height() + 10;
    const QRect chip((width() - fm.horizontalAdvance(hint_) - 20) / 2, 10,
                     fm.horizontalAdvance(hint_) + 20, chip_h);
    painter.setPen(Qt::NoPen);
    // Translucent canvas rather than a new colour: the chip has to stay readable
    // over whatever the remote desktop happens to be showing underneath.
    QColor scrim = c.canvas;
    scrim.setAlpha(210);
    painter.setBrush(scrim);
    painter.drawRoundedRect(chip, 4, 4);
    painter.setPen(c.text);
    painter.drawText(chip, Qt::AlignCenter, hint_);
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

void DisplayRenderer::set_hint(QString text) {
    if (hint_ == text) {
        return;
    }
    hint_ = std::move(text);
    update();
}
