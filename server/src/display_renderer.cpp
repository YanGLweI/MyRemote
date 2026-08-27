#include "display_renderer.hpp"

#include <QPainter>

DisplayRenderer::DisplayRenderer(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 240);
    setStyleSheet("background-color: #1a1a1a;");
}

bool DisplayRenderer::has_frame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !current_.isNull();
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

    // Letterbox into the widget while keeping aspect ratio.
    QSize scaled = frame.size().scaled(size(), Qt::KeepAspectRatio);
    QRect target((width() - scaled.width()) / 2, (height() - scaled.height()) / 2,
                 scaled.width(), scaled.height());
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(target, frame);
}
