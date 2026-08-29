#include "display_renderer.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

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
    painter.fillRect(rect(), QColor(26, 26, 26));
    QImage frame;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frame = current_;
    }
    if (frame.isNull()) {
        painter.setPen(QColor(120, 120, 120));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("等待远程画面…"));
    } else {
        QRect target = remote_rect();
        if (target.isEmpty()) {
            target = rect();
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
    painter.setBrush(QColor(12, 14, 16, 210));
    painter.drawRoundedRect(chip, 4, 4);
    painter.setPen(QColor(232, 236, 240));
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
