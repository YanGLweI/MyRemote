#include "device_row_delegate.hpp"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>

#include "device_list_model.hpp"

namespace {

// The four states a row can be in, and the only colours this delegate owns.
// Everything else comes from the palette so the theme still drives it.
QColor state_color(DeviceState state) {
    switch (state) {
        case DeviceState::Live:
            return QColor(0x3E, 0x9B, 0x6E);
        case DeviceState::Reconnecting:
            return QColor(0xD9, 0xA0, 0x3C);
        case DeviceState::Offline:
            return QColor(0x6E, 0x76, 0x81);
    }
    return QColor();
}

constexpr int kHPadding = 10;
constexpr int kVPadding = 7;
constexpr int kDot = 9;
constexpr int kGap = 9;

}  // namespace

QFont DeviceRowDelegate::meta_font(const QFont& base) {
    QFont mono = base;
    mono.setFamilies({QStringLiteral("Consolas"), QStringLiteral("Cascadia Mono"),
                      QStringLiteral("Courier New")});
    mono.setPointSizeF(base.pointSizeF() - 1.0);
    return mono;
}

int DeviceRowDelegate::row_height(const QFont& base) {
    const QFontMetrics name(base), meta(meta_font(base));
    return kVPadding * 2 + name.height() + meta.height();
}

QSize DeviceRowDelegate::sizeHint(const QStyleOptionViewItem& option,
                                  const QModelIndex&) const {
    return QSize(220, row_height(option.font));
}

void DeviceRowDelegate::paint(QPainter* painter,
                              const QStyleOptionViewItem& option,
                              const QModelIndex& index) const {
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;

    // Let the style draw the base, selection and focus; this is where a widget
    // would get those for free.
    QStyleOptionViewItem base = option;
    initStyleOption(&base, index);
    base.text.clear();
    base.icon = QIcon();
    if (!selected && hovered) {
        base.backgroundBrush = QBrush(option.palette.color(QPalette::Mid)
                                           .lighter(108));
    }
    option.widget->style()->drawControl(QStyle::CE_ItemViewItem, &base, painter,
                                        option.widget);

    const DeviceState state = static_cast<DeviceState>(
        index.data(DeviceListModel::StateRole).toInt());
    const QString name = index.data(DeviceListModel::NameRole).toString();
    const QString meta = index.data(DeviceListModel::MetaRole).toString();
    const QStringList badges = index.data(DeviceListModel::BadgesRole).toStringList();

    QColor text = option.palette.color(
        selected ? QPalette::HighlightedText : QPalette::Text);
    QColor muted = option.palette.color(QPalette::PlaceholderText);
    if (state == DeviceState::Offline) {
        text = muted;  // a remembered row should look remembered
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    const QRect body = option.rect.adjusted(kHPadding, kVPadding, -kHPadding,
                                            -kVPadding);
    QFont name_font = option.font;
    name_font.setBold(true);
    const QFontMetrics name_fm(name_font);
    const int name_h = name_fm.height();
    const QFont mono = meta_font(option.font);
    const QFontMetrics mono_fm(mono);

    // State dot, centred on the name line so it marks the machine rather than
    // one of its facts. Offline is a ring: there is nothing to light up.
    const QRectF dot(body.left(), body.top() + (name_h - kDot) / 2.0, kDot, kDot);
    const QColor edge = state_color(state);
    if (state == DeviceState::Offline) {
        QPen ring(muted);
        ring.setWidthF(1.4);
        painter->setPen(ring);
        painter->setBrush(Qt::NoBrush);
    } else {
        painter->setPen(QPen(edge.darker(140), 1));
        painter->setBrush(QBrush(edge));
    }
    painter->drawEllipse(dot);

    const int text_left = static_cast<int>(dot.right()) + kGap;
    const int line_right = option.rect.right() - kHPadding;
    QRect name_line(text_left, body.top(), line_right - text_left, name_h);

    // Chips are measured first: a long hostname must never push the capability
    // badges off the row.
    QFont chip_font = option.font;
    chip_font.setPointSizeF(option.font.pointSizeF() - 2.0);
    const QFontMetrics chip_fm(chip_font);
    const int chip_h = chip_fm.height() + 4;
    int chips_w = 0;
    for (const QString& badge : badges) {
        chips_w += chip_fm.horizontalAdvance(badge) + 12 + 4;
    }
    const int name_room =
        qMax(40, name_line.width() - (chips_w ? chips_w - 4 : 0));
    const QString shown_name =
        name_fm.elidedText(name, Qt::ElideRight, name_room);
    painter->setFont(name_font);
    painter->setPen(text);
    painter->drawText(name_line, Qt::AlignLeft | Qt::AlignVCenter, shown_name);

    int chip_x = name_line.left() + name_fm.horizontalAdvance(shown_name);
    painter->setFont(chip_font);
    for (const QString& badge : badges) {
        const int w = chip_fm.horizontalAdvance(badge) + 12;
        QRectF chip(chip_x, name_line.top() + (name_h - chip_h) / 2.0, w, chip_h);
        if (chip.right() > line_right) {
            break;
        }
        QColor chip_edge = edge;
        chip_edge.setAlpha(150);
        painter->setPen(QPen(chip_edge, 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(chip, chip_h / 2.0, chip_h / 2.0);
        painter->setPen(muted);
        painter->drawText(chip, Qt::AlignCenter, badge);
        chip_x += w + 4;
    }

    const QRect meta_line(text_left, body.top() + name_h, line_right - text_left,
                          mono_fm.height());
    painter->setFont(mono);
    painter->setPen(muted);
    painter->drawText(meta_line, Qt::AlignLeft | Qt::AlignVCenter,
                      mono_fm.elidedText(meta, Qt::ElideRight, meta_line.width()));
    painter->restore();
}
