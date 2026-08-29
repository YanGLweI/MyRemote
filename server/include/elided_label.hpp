#pragma once

#include <QLabel>

// A label whose text is the thing that gives way, never the window's width.
//
// A plain QLabel reports its full text as minimumSizeHint, so a label whose
// text changes at runtime silently becomes the layout's minimum width - and one
// layout away from a splitter, that is the neighbouring pane's width. QSizePolicy
// cannot express what is wanted here: Ignored also throws away the preferred
// width, so the label collapses to nothing.
//
// So keep the full text (sizeHint stays honest, and the label is given its
// natural width whenever there is room), lower only the floor, and ellipsise at
// paint time when the room is not there.
class ElidedLabel : public QLabel {
    Q_OBJECT

public:
    explicit ElidedLabel(QWidget* parent = nullptr);

    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
};
