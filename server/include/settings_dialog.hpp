#pragma once

#include <QDialog>
#include <QKeySequence>

#include "config.hpp"

class QLabel;
class QLineEdit;
class QKeySequenceEdit;

// The window-level knobs: which keystroke hands the keyboard back, where the
// running record goes, and how the listener is armed.
//
// A pure form. It reads what is configured, refuses inputs that cannot work,
// and decides nothing — applying and persisting stay with the window, so a
// dialog that was cancelled cannot have changed anything behind its back.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    SettingsDialog(const config::ServerConfig& cfg, const QKeySequence& release_key,
                   QWidget* parent = nullptr);

    // Only valid after the dialog was accepted.
    QKeySequence release_key() const;
    // As written: a bare name means next to the program, an empty field means
    // no file at all.
    QString log_file() const;
    QString bind_address() const;
    int listening_port() const;
    int max_connections() const;

protected:
    void accept() override;

private:
    QKeySequenceEdit* release_ = nullptr;
    QLineEdit* log_ = nullptr;
    QLineEdit* bind_ = nullptr;
    QLineEdit* port_ = nullptr;
    QLineEdit* connections_ = nullptr;
    QLabel* log_state_ = nullptr;
};
