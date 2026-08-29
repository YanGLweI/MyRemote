#pragma once

#include <QIcon>

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

}  // namespace icons
