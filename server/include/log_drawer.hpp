#pragma once

#include <QWidget>

class LogTail;
class QCheckBox;
class QPlainTextEdit;
class QPushButton;

// The control centre's own running record: registrations, tunnels, decoders,
// the reasons a session went away. The file has all of it; this keeps the last
// few hundred lines where the operator can read them next to the thing that
// just happened.
class LogDrawer : public QWidget {
    Q_OBJECT

public:
    explicit LogDrawer(LogTail& tail, QWidget* parent = nullptr);

signals:
    // Warn and Error lines only, so the exit in the status bar can count the
    // ones worth opening the drawer for.
    void problem_count_changed(int count);

public slots:
    void append_line(QString line, int level);

private:
    bool at_bottom() const;

    QPlainTextEdit* view_ = nullptr;
    QCheckBox* follow_ = nullptr;
    QPushButton* clear_button_ = nullptr;
    int problems_ = 0;
};
