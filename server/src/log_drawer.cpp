#include "log_drawer.hpp"

#include <QBrush>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

#include "log.hpp"
#include "log_tail.hpp"
#include "theme.hpp"

namespace {
// The file keeps everything; the drawer only has to be long enough to see what
// just happened. An unbounded QPlainTextEdit is a leak that grows with uptime.
constexpr int kKeepLines = 500;

QColor colour_for(int level) {
    switch (static_cast<mlog::Level>(level)) {
        case mlog::Level::Error: return theme::colors().denied;
        case mlog::Level::Warn: return theme::colors().stale;
        default: return theme::colors().muted;
    }
}
}  // namespace

LogDrawer::LogDrawer(LogTail& tail, QWidget* parent) : QWidget(parent) {
    view_ = new QPlainTextEdit();
    view_->setReadOnly(true);
    view_->setFrameShape(QFrame::NoFrame);
    view_->setFont(theme::meta_font());
    // Wrapping a 200-character path turns one fact into four lines and the
    // drawer stops being scannable; scroll sideways instead.
    view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    view_->document()->setMaximumBlockCount(kKeepLines);

    clear_button_ = new QPushButton(QStringLiteral("清空"));
    clear_button_->setToolTip(QStringLiteral("只清掉这里的显示，日志文件不动。"));
    follow_ = new QCheckBox(QStringLiteral("跟随末尾"));
    follow_->setChecked(true);
    follow_->setToolTip(QStringLiteral("新行到达时滚到最底下。往回翻会自动取消。"));

    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(8, 4, 8, 4);
    bar->setSpacing(12);
    bar->addWidget(clear_button_);
    bar->addWidget(follow_);
    bar->addStretch(1);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addLayout(bar);
    root->addWidget(view_, 1);

    connect(&tail, &LogTail::line_logged, this, &LogDrawer::append_line);
    connect(clear_button_, &QPushButton::clicked, this, [this] {
        view_->clear();
        problems_ = 0;
        emit problem_count_changed(0);
    });
    // Reading backwards must not be fought by the next line arriving.
    connect(view_->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { if (!at_bottom()) follow_->setChecked(false); });
}

bool LogDrawer::at_bottom() const {
    const QScrollBar* bar = view_->verticalScrollBar();
    return bar->value() >= bar->maximum() - 2;
}

void LogDrawer::append_line(QString line, int level) {
    if (static_cast<mlog::Level>(level) != mlog::Level::Info) {
        ++problems_;
        emit problem_count_changed(problems_);
    }
    QTextCursor cursor(view_->document());
    cursor.movePosition(QTextCursor::End);
    QTextCharFormat format;
    format.setForeground(QBrush(colour_for(level)));
    cursor.insertText(line + QStringLiteral("\n"), format);
    if (follow_->isChecked()) {
        QScrollBar* bar = view_->verticalScrollBar();
        bar->setValue(bar->maximum());
    }
}
