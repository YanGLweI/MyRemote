#pragma once

#include <QIcon>
#include <QString>

// Marks for this console, drawn rather than shipped. The Qt kit has no Svg
// module and the app carries no .qrc, so a resource icon set is not an option;
// and a bitmap authored at one scale goes soft on the ~200% panel this runs on,
// which is why every glyph is rasterised at the screen's own device pixel ratio.
namespace icons {

QIcon search();      // the roster filter: named by its mark, not by a second label
QIcon fullscreen();  // expand the picture into the window
QIcon restore();     // put it back in its box
QIcon lock();        // hand the workstation back to its logon screen
QIcon close();       // a tab's exit: the one control that must never be missed

// A stylesheet can only be given a sub-control's image as a file name, so the
// combo box's arrow is drawn out to the temp dir and named from there. Empty
// means nothing could be written, and the caller leaves the rule out instead.
QString down_arrow_url();

}  // namespace icons
