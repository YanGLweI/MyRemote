#include "elided_label.hpp"

#include <QFontMetrics>
#include <QPainter>

ElidedLabel::ElidedLabel(QWidget* parent) : QLabel(parent) {}

QSize ElidedLabel::minimumSizeHint() const {
    // Four digits of floor: enough to stay legible when the pane is squeezed,
    // and nowhere near enough to force the window wider.
    const QFontMetrics fm(font());
    return QSize(fm.horizontalAdvance(QLatin1Char('0')) * 4,
                 QLabel::sizeHint().height());
}

void ElidedLabel::paintEvent(QPaintEvent* event) {
    if (width() >= QLabel::sizeHint().width()) {
        QLabel::paintEvent(event);  // there is room: draw it exactly as before
        return;
    }
    QRect r = contentsRect();
    const int margin = QLabel::margin();
    if (margin > 0) {
        r.adjust(margin, margin, -margin, -margin);
    }
    QPainter painter(this);
    painter.setPen(palette().color(foregroundRole()));
    painter.drawText(r, alignment(),
                     fontMetrics().elidedText(text(), Qt::ElideRight, r.width()));
}
