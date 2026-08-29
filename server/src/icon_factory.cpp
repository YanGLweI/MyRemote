#include "icon_factory.hpp"

#include <QDir>
#include <QGuiApplication>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>
#include <QScreen>
#include <QSize>

#include <functional>

#include "theme.hpp"

namespace {

constexpr int kSide = 16;
constexpr qreal kArm = 4.0;

using Drawing = std::function<void(QPainter&, const QColor&)>;

QPixmap rasterise(const Drawing& draw, const QColor& ink) {
    // Rasterised once for the screen the console lives on. The ratio is read from
    // the screen rather than a widget because a widget still reports 1 until it
    // has a native window, which is before the first icon is ever asked for.
    const qreal dpr =
        qMax<qreal>(1.0, QGuiApplication::primaryScreen()->devicePixelRatio());
    QPixmap pm(QSize(kSide, kSide) * dpr);
    pm.setDevicePixelRatio(dpr);  // painting is then in logical units
    pm.fill(Qt::transparent);
    QPainter g(&pm);
    g.setRenderHint(QPainter::Antialiasing);
    QPen pen(ink);
    pen.setWidthF(1.6);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    g.setPen(pen);
    g.setBrush(Qt::NoBrush);
    draw(g, ink);
    g.end();
    return pm;
}

QIcon make(const Drawing& draw) {
    QIcon icon;
    // Muted at rest, with a lit copy for the styles that swap to Active on hover.
    // These marks sit next to their own label and must not compete with it.
    icon.addPixmap(rasterise(draw, theme::colors().muted), QIcon::Normal);
    icon.addPixmap(rasterise(draw, theme::colors().text), QIcon::Active);
    return icon;
}

// An L with its vertex at (x, y) opening toward (dx, dy).
void bracket(QPainter& g, qreal x, qreal y, qreal dx, qreal dy) {
    g.drawPolyline(QPolygonF() << QPointF(x + dx * kArm, y) << QPointF(x, y)
                               << QPointF(x, y + dy * kArm));
}

QRectF bounds() { return QRectF(2, 2, kSide - 4, kSide - 4); }

}  // namespace

namespace icons {

QIcon search() {
    return make([](QPainter& g, const QColor&) {
        g.drawEllipse(QRectF(3.0, 3.0, 7.0, 7.0));
        // The handle starts on the lens's own radius, or the two read as
        // separate objects at this size.
        g.drawLine(QPointF(9.5, 9.5), QPointF(13.2, 13.2));
    });
}

QIcon fullscreen() {
    return make([](QPainter& g, const QColor&) {
        const QRectF r = bounds();
        bracket(g, r.left(), r.top(), 1, 1);
        bracket(g, r.right(), r.top(), -1, 1);
        bracket(g, r.left(), r.bottom(), 1, -1);
        bracket(g, r.right(), r.bottom(), -1, -1);
    });
}

QIcon restore() {
    // The same four brackets, pulled in and turned around: the picture back
    // inside its box rather than filling it.
    return make([](QPainter& g, const QColor&) {
        const QRectF r = bounds().adjusted(4, 4, -4, -4);
        bracket(g, r.left(), r.top(), -1, -1);
        bracket(g, r.right(), r.top(), 1, -1);
        bracket(g, r.left(), r.bottom(), -1, 1);
        bracket(g, r.right(), r.bottom(), 1, 1);
    });
}

QIcon lock() {
    return make([](QPainter& g, const QColor&) {
        // Shackle endpoints land on the body's top edge, or the two read as
        // separate objects at this size.
        g.drawArc(QRectF(5.5, 3.2, 5.0, 6.4), 0, 180 * 16);
        g.drawRoundedRect(QRectF(3.6, 6.4, 8.8, 7.0), 1.6, 1.6);
    });
}

QIcon close() {
    return make([](QPainter& g, const QColor&) {
        const qreal inset = 4.5;
        g.drawLine(QPointF(inset, inset), QPointF(kSide - inset, kSide - inset));
        g.drawLine(QPointF(kSide - inset, inset), QPointF(inset, kSide - inset));
    });
}

QString down_arrow_url() {
    static const QString path = [] {
        const QString file =
            QDir::tempPath() + QStringLiteral("/myremote-down-arrow.png");
        const QPixmap pm = rasterise(
            [](QPainter& g, const QColor&) {
                g.drawPolyline(QPolygonF() << QPointF(4.0, 6.5)
                                           << QPointF(8.0, 10.0)
                                           << QPointF(12.0, 6.5));
            },
            theme::colors().muted);
        if (!pm.save(file, "PNG")) {
            return QString();
        }
        return QDir::fromNativeSeparators(file);
    }();
    return path;
}

}  // namespace icons
