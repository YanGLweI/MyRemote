#include "settings_dialog.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "log.hpp"

SettingsDialog::SettingsDialog(const config::ServerConfig& cfg,
                               const QKeySequence& release_key, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("设置"));
    auto* root = new QVBoxLayout(this);

    auto* keyboard = new QGroupBox(QStringLiteral("键盘"));
    auto* keyboard_form = new QFormLayout(keyboard);
    release_ = new QKeySequenceEdit(release_key);
    release_->setToolTip(QStringLiteral(
        "按这组键把键盘交还本机，不用点任何东西。\n"
        "必须是一起按下的几个键，不能是先后按的两串。\n"
        "Win、Alt+F4、Alt+Esc 不能当释放热键：它们设计上就留在本机。\n"
        "留空＝只能用连按两次 Esc 交还。"));
    // The editor cannot be emptied from the keyboard - Backspace, Delete and Esc
    // are all recorded as a new hotkey instead of clearing it - so taking the
    // hotkey away needs a button of its own.
    auto* clear_release = new QPushButton(QStringLiteral("清除"));
    clear_release->setToolTip(QStringLiteral("不要释放热键，只用连按两次 Esc 交还键盘。"));
    connect(clear_release, &QPushButton::clicked, release_, &QKeySequenceEdit::clear);
    auto* release_row = new QWidget();
    auto* release_line = new QHBoxLayout(release_row);
    release_line->setContentsMargins(0, 0, 0, 0);
    release_line->addWidget(release_, 1);
    release_line->addWidget(clear_release, 0);
    // Qt's own prompt for an empty editor is the English "Press shortcut", so it
    // has to be said in Chinese here - the widget exposes no placeholder of its
    // own and keeps the text in the line edit it builds for itself. Setting it
    // once is not enough: measured, clearing the sequence brings the English
    // wording back, because the editor rewrites its own prompt on every change.
    if (auto* prompt = release_->findChild<QLineEdit*>()) {
        const QString wording = QStringLiteral("按下组合键");
        prompt->setPlaceholderText(wording);
        connect(release_, &QKeySequenceEdit::keySequenceChanged, this,
                [prompt, wording] { prompt->setPlaceholderText(wording); });
    }
    keyboard_form->addRow(QStringLiteral("释放热键"), release_row);
    root->addWidget(keyboard);

    auto* record = new QGroupBox(QStringLiteral("运行记录"));
    auto* record_form = new QFormLayout(record);
    log_ = new QLineEdit(QString::fromStdString(cfg.log_file));
    log_->setToolTip(QStringLiteral(
        "写在程序目录里的文件名（例如 control_server.log），或者一个完整路径。\n"
        "留空＝不写文件，运行记录只在窗口下方事件日志里看。"));
    record_form->addRow(QStringLiteral("日志文件"), log_);
    // What the log is doing right now, not what was last asked for: the field
    // above can name a file this run is not writing to, and saying so is the
    // only way the two readings cannot be confused with each other.
    const QString current = QString::fromStdString(mlog::path());
    log_state_ = new QLabel(current.isEmpty()
                                ? QStringLiteral("当前写入：无，这次只在窗口里记")
                                : QStringLiteral("当前写入：%1").arg(current));
    log_state_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // A clipped path would hide exactly the fact this line exists to show, so it
    // wraps instead - while staying out of the dialog's minimum width.
    log_state_->setWordWrap(true);
    log_state_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    record_form->addRow(log_state_);
    root->addWidget(record);

    auto* inbound = new QGroupBox(QStringLiteral("接入"));
    auto* inbound_form = new QFormLayout(inbound);
    port_ = new QLineEdit(QString::number(cfg.listening_port));
    port_->setValidator(new QIntValidator(1, 65535, port_));
    port_->setToolTip(QStringLiteral("被控端拨进来的端口。"));
    bind_ = new QLineEdit(QString::fromStdString(cfg.bind_address));
    bind_->setToolTip(QStringLiteral("0.0.0.0 ＝本机所有网卡；也可以只写一个地址。"));
    connections_ = new QLineEdit(QString::number(cfg.max_connections));
    connections_->setValidator(new QIntValidator(1, 4096, connections_));
    connections_->setToolTip(QStringLiteral("同时保持的隧道上限，多出来的连接会被拒。"));
    inbound_form->addRow(QStringLiteral("监听端口"), port_);
    inbound_form->addRow(QStringLiteral("绑定地址"), bind_);
    inbound_form->addRow(QStringLiteral("最大接入数"), connections_);
    auto* restart = new QLabel(QStringLiteral("这三项要重启控制端才生效。"));
    restart->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    inbound_form->addRow(restart);
    root->addWidget(inbound);

    // The listening secret is deliberately not here: changing it silently orphans
    // every machine already registered, which is a job for whoever can reach all
    // of them at once, not a click in a window.
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

QKeySequence SettingsDialog::release_key() const {
    const QKeySequence seq = release_->keySequence();
    // Only the first combination can ever fire: the gateway compares one
    // keystroke against this, so a second step would be a hotkey that never works.
    return seq.count() > 0 ? QKeySequence(seq[0]) : QKeySequence();
}

QString SettingsDialog::log_file() const { return log_->text().trimmed(); }

QString SettingsDialog::bind_address() const { return bind_->text().trimmed(); }

int SettingsDialog::listening_port() const { return port_->text().toInt(); }

int SettingsDialog::max_connections() const { return connections_->text().toInt(); }

void SettingsDialog::accept() {
    const QKeySequence typed = release_->keySequence();
    if (typed.count() > 1) {
        // Taking the first step would be worse than refusing: "Ctrl+A, Del"
        // would come back as a hotkey that fires on select-all.
        QMessageBox::warning(this, QStringLiteral("设置"), QStringLiteral(
            "释放热键必须是一起按下的几个键，不能是先后按的两串。"));
        release_->setFocus();
        return;
    }
    const QKeySequence key = release_key();
    if (!key.isEmpty()) {
        switch (key[0].key()) {
            case Qt::Key_Control:
            case Qt::Key_Shift:
            case Qt::Key_Alt:
            case Qt::Key_AltGr:
            case Qt::Key_Meta:
            case Qt::Key_unknown:
                QMessageBox::warning(this, QStringLiteral("设置"), QStringLiteral(
                    "释放热键要在修饰键之外再按下一个键，例如 Ctrl+Alt+Shift+R。"));
                release_->setFocus();
                return;
            default:
                break;
        }
    }
    if (port_->text().isEmpty() || connections_->text().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("设置"),
                             QStringLiteral("端口和最大接入数都要填一个数字。"));
        (port_->text().isEmpty() ? port_ : connections_)->setFocus();
        return;
    }
    QDialog::accept();
}
