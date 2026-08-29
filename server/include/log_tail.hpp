#pragma once

#include <QObject>
#include <QString>

// Carries the control centre's own log lines into the window. mlog writes from
// whatever thread hit the event - a tunnel session thread, the decode thread,
// the GUI thread - so nothing here touches a widget: the line is queued and
// whoever listens decides what to show. Installed before the first device is
// registered so the lines that explain a session are already there when the
// operator opens the drawer.
class LogTail : public QObject {
    Q_OBJECT

public:
    explicit LogTail(QObject* parent = nullptr);
    // Detaches from mlog: a writer thread holding the sink after this object is
    // gone would be a use-after-free, not a missing log line.
    ~LogTail() override;

signals:
    // Emitted on the GUI thread only. int is an mlog::Level.
    void line_logged(QString line, int level);
};
