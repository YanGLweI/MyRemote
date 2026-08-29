#include "device_row_delegate.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>

#include "device_list_model.hpp"
#include "theme.hpp"

namespace {

constexpr int kHPadding = 10;
constexpr int kVPadding = 7;
constexpr int kDot = 9;
constexpr int kGap = 9;

QColor state_color(DeviceState state) {
    switch (state) {
        case DeviceState::Live:
            return theme::colors().live;
        case DeviceState::Reconnecting:
            return theme::colors().reconnecting;
        case DeviceState::Offline:
            return theme::colors().stale;
    }
    return theme::colors().stale;
}

QFont name_font() {
    QFont f = theme::base_font();
    f.setBold(true);
    return f;
}

int line_height(const QFont& font) { return QFontMetrics(font).height(); }

}  // namespace

QSize DeviceRowDelegate::sizeHint(const QStyleOptionViewItem& option,
                                  const QModelIndex&) const {
    const int height = kVPadding * 2 + line_height(name_font()) +
                       line_height(theme::meta_font());
    return QSize(220, height);
}

void DeviceRowDelegate::paint(QPainter* painter,
                              const QStyleOptionViewItem& option,
                              const QModelIndex& index) const {
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;

    // Let the style draw the base, the selection and the focus ring: this is
    // where a widget would get them for free.
    QStyleOptionViewItem base = option;
    initStyleOption(&base, index);
    base.text.clear();
    base.icon = QIcon();
    // The focus rectangle is drawn in the highlight colour, which would paint
    // every row the operator has merely tabbed past as if it were chosen.
    base.state &= ~QStyle::State_HasFocus;
    if (selected) {
        base.backgroundBrush = QBrush(theme::colors().surface_hover);
    } else if (hovered) {
        // Lighter than a selection on purpose: the pointer is transient, the
        // choice is not.
        QColor plate = theme::colors().surface_hover;
        plate.setAlpha(110);
        base.backgroundBrush = QBrush(plate);
    }
    option.widget->style()->drawControl(QStyle::CE_ItemViewItem, &base, painter,
                                        base.widget);

    const DeviceState state = static_cast<DeviceState>(
        index.data(DeviceListModel::StateRole).toInt());
    const QString name = index.data(DeviceListModel::NameRole).toString();
    const QString meta = index.data(DeviceListModel::MetaRole).toString();
    const QStringList badges = index.data(DeviceListModel::BadgesRole).toStringList();

    const bool dimmed = state == DeviceState::Offline;
    // A remembered row should look remembered; selection is a border plus a
    // surface here, not an inversion, so the name keeps its own colour either way.
    QColor text = dimmed ? theme::colors().stale : theme::colors().text;
    const QColor muted = theme::colors().muted;
    const QFontMetrics name_fm(name_font());
    const QFontMetrics mono_fm(theme::meta_font());
    const QFontMetrics chip_fm(theme::badge_font());
    const int name_h = name_fm.height();

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    if (selected) {
        // The bar is the mark of a choice. The plate behind it is shared with
        // hovering, so a moving pointer cannot imitate a selection.
        painter->setPen(Qt::NoPen);
        painter->setBrush(theme::colors().accent);
        painter->drawRect(QRectF(option.rect.left(), option.rect.top(), 2,
                                 option.rect.height()));
    }

    const QRect body = option.rect.adjusted(kHPadding, kVPadding, -kHPadding,
                                            -kVPadding);

    // State dot, centred on the name line so it marks the machine rather than one
    // of its facts. Offline is a ring: there is nothing lit to show.
    const QRectF dot(body.left(), body.top() + (name_h - kDot) / 2.0, kDot, kDot);
    const QColor edge = state_color(state);
    if (dimmed) {
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
    const QRect name_line(text_left, body.top(), line_right - text_left, name_h);

    // Chips are measured first: a long hostname must never push the capability
    // badges off the row.
    int chips_w = 0;
    for (const QString& badge : badges) {
        chips_w += chip_fm.horizontalAdvance(badge) + 12 + 4;
    }
    const int name_room =
        qMax(40, name_line.width() - (chips_w ? chips_w - 4 : 0));
    const QString shown_name =
        name_fm.elidedText(name, Qt::ElideRight, name_room);
    painter->setFont(name_font());
    painter->setPen(text);
    painter->drawText(name_line, Qt::AlignLeft | Qt::AlignVCenter, shown_name);

    int chip_x = name_line.left() + name_fm.horizontalAdvance(shown_name);
    const int chip_h = chip_fm.height() + 4;
    painter->setFont(theme::badge_font());
    for (const QString& badge : badges) {
        const int w = chip_fm.horizontalAdvance(badge) + 12;
        QRectF chip(chip_x, name_line.top() + (name_h - chip_h) / 2.0, w, chip_h);
        if (chip.right() > line_right) {
            break;
        }
        QColor chip_edge = dimmed ? muted : edge;
        chip_edge.setAlpha(selected ? 220 : 150);
        painter->setPen(QPen(chip_edge, 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(chip, chip_h / 2.0, chip_h / 2.0);
        painter->setPen(muted);
        painter->drawText(chip, Qt::AlignCenter, badge);
        chip_x += w + 4;
    }

    const QRect meta_line(text_left, body.top() + name_h, line_right - text_left,
                          mono_fm.height());
    painter->setFont(theme::meta_font());
    painter->setPen(dimmed ? muted : theme::colors().muted);
    painter->drawText(meta_line, Qt::AlignLeft | Qt::AlignVCenter,
                      mono_fm.elidedText(meta, Qt::ElideRight, meta_line.width()));
    painter->restore();
}
