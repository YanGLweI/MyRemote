#pragma once

#include <QColor>
#include <QFont>
#include <QString>

class QApplication;
class QWidget;

// The console's one source of colour, type and control shape.
//
// Two mechanisms, deliberately split: the palette carries colour (so custom
// painting in DeviceRowDelegate and DisplayRenderer reads the same values as the
// built-in styles), while the stylesheet only shapes the interactive controls
// that the native style renders in Windows colours. Widening the stylesheet to
// colour plain QWidgets would override both the palette and the widgets that
// paint themselves.
namespace theme {

struct Palette {
    QColor bg;              // window
    QColor canvas;          // letterbox behind a remote desktop: darker than bg, so
                            // the picture reads as lifted rather than as another panel
    QColor surface;         // panels, inputs
    QColor surface_raised;  // controls, hover, selection-free chrome
    QColor surface_hover;
    QColor line;
    QColor text;
    QColor muted;
    QColor accent;
    // State colours are meaning, not decoration: a row, a pill and a dot have to
    // agree on what amber means.
    QColor live;
    QColor reconnecting;
    QColor stale;
    QColor denied;
};

const Palette& colors();

QFont base_font();
QFont section_font();   // a step up and semibold: names, headings
QFont meta_font();      // monospaced: machine facts, counters
QFont badge_font();     // a step down: chips inside a row

void apply(QApplication& app);
QString stylesheet();

// Give one widget its own text colour. A palette change is not enough: the
// application stylesheet inherits a `color` to every child and wins over it.
void tint(QWidget* w, const QColor& c);

}  // namespace theme
