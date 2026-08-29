#pragma once

#include <QStyledItemDelegate>

// Paints one roster row: a state dot, the name the operator uses, real badge
// chips, and a monospaced line of machine facts. Everything comes from model
// roles, so the row costs no widgets.
class DeviceRowDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

private:
    static QFont meta_font(const QFont& base);
    static int row_height(const QFont& base);
};
