#include "session_toolbar.hpp"

#include <QComboBox>
#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "elided_label.hpp"
#include "icon_factory.hpp"
#include "remote_controller.hpp"
#include "theme.hpp"

namespace {

// Text that changes size at runtime must never drive the layout. A QLabel's
// minimumSizeHint is its own text width, so an unbounded 帧率 counter raises
// this tab's minimum width once per second and the splitter pays for it out of
// the roster: that reads as the whole window shaking. So every label whose words
// come and go is pinned to the width of its widest possible string.
void reserve(QWidget* w, const QString& widest) {
    w->setFixedWidth(w->fontMetrics().horizontalAdvance(widest) + 8);
}

void reserve_button(QPushButton* b, const QString& widest) {
    const QString current = b->text();
    b->setText(widest);
    const int width = b->sizeHint().width();
    b->setText(current);
    b->setMinimumWidth(width);
}

}  // namespace

SessionToolbar::SessionToolbar(QWidget* parent) : QWidget(parent) {
    auto* box = new QVBoxLayout(this);
    box->setContentsMargins(8, 5, 8, 5);
    box->setSpacing(3);

    auto* actions = new QHBoxLayout();
    actions->setSpacing(8);
    auto* status = new QHBoxLayout();
    status->setSpacing(8);
    box->addLayout(actions);
    box->addLayout(status);

    title_ = new ElidedLabel();
    QFont bold = title_->font();
    bold.setBold(true);
    title_->setFont(bold);
    title_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    quality_ = new QComboBox();
    for (int i = 0; i < kQualityPresetCount; ++i) {
        quality_->addItem(QString::fromUtf8(kQualityPresets[i].label));
        quality_->setItemData(i, QString::fromUtf8(kQualityPresets[i].spec),
                              Qt::ToolTipRole);
    }
    quality_->setCurrentIndex(kQualityDefault);
    quality_->setToolTip(QStringLiteral("画质档位（只作用于这台设备）"));

    logon_button_ = new QPushButton(QStringLiteral("返回登录界面"));
    logon_button_->setIcon(icons::lock());
    logon_button_->setEnabled(false);
    logon_button_->setToolTip(QStringLiteral(
        "锁定工作站，让对端回到登录界面后可以远程输入密码登录。\n"
        "Ctrl+Alt+Del 无法被注入：若策略要求按 Ctrl+Alt+Del 才开始登录，\n"
        "请将该机器的 DisableCAD 设为 1。"));

    fullscreen_button_ = new QPushButton(QStringLiteral("全屏"));
    fullscreen_button_->setIcon(icons::fullscreen());
    fullscreen_button_->setCheckable(true);
    fullscreen_button_->setToolTip(QStringLiteral(
        "让这台设备铺满整个窗口。标签栏和这一条工具栏都会留在画面上方，\n"
        "出口一直在屏幕上。"));

    stop_button_ = new QPushButton(QStringLiteral("断开"));

    state_ = new QLabel();
    capture_ = new QLabel();
    capture_->setText(QStringLiteral("键盘 · 本地"));
    refresh_capture_tip();
    detail_ = new ElidedLabel();
    detail_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    stats_ = new QLabel(QStringLiteral("帧率 -- · 延迟 --"));
    stats_->setToolTip(QStringLiteral(
        "帧率＝每秒真正解码并画到屏幕上的帧数；它低于画质档位的设定值，说明对端或链\n"
        "路没发够。对端桌面静止时不产帧，显示 0 是正常的。\n"
        "延迟＝本机到对端的一次网络往返，每秒测一次，与画面是否在动无关。它不含采集、\n"
        "编码与解码，只反映链路。显示 -- 表示还没收到回答，或对端版本不认这个探测。"));

    actions->addWidget(title_, 1);
    actions->addWidget(quality_);
    actions->addWidget(logon_button_);
    actions->addWidget(fullscreen_button_);
    actions->addWidget(stop_button_);

    status->addWidget(state_);
    status->addWidget(capture_);
    status->addWidget(detail_, 1);
    status->addWidget(stats_);

    reserve(state_, QStringLiteral("重连中"));
    reserve(capture_, QStringLiteral("键盘 · 远程"));
    reserve(stats_, QStringLiteral("帧率 100 · 延迟 1234ms"));
    stats_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    reserve_button(stop_button_, QStringLiteral("重新连接"));
    reserve_button(fullscreen_button_, QStringLiteral("退出全屏"));

    connect(quality_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int idx) { emit quality_selected(idx); });
    connect(fullscreen_button_, &QPushButton::clicked, this,
            [this](bool on) { emit fullscreen_toggled(on); });
    connect(stop_button_, &QPushButton::clicked, this, [this]() {
        if (streaming_) {
            emit stop_requested();
        } else {
            emit start_requested();
        }
    });
    connect(logon_button_, &QPushButton::clicked, this,
            &SessionToolbar::logon_requested);
}

void SessionToolbar::set_title(const QString& device_name, const QString& detail) {
    title_->setText(device_name);
    title_->setToolTip(device_name);
    detail_->setText(detail);
    detail_->setToolTip(detail);
}

void SessionToolbar::set_state_text(const QString& text, bool live) {
    state_->setText(text);
    theme::tint(state_, live ? theme::colors().live : theme::colors().reconnecting);
}

void SessionToolbar::set_streaming(bool on) {
    streaming_ = on;
    // One button, two honest labels: what it says is what it does.
    stop_button_->setText(on ? QStringLiteral("断开")
                             : QStringLiteral("重新连接"));
    capture_->setVisible(on);
    if (!on) {
        stats_->setText(QStringLiteral("帧率 -- · 延迟 --"));
    }
}

void SessionToolbar::set_supports_logon(bool on) {
    logon_button_->setEnabled(on && streaming_);
}

void SessionToolbar::refresh_capture_tip() {
    const QString combo = release_key_.toString();
    const QString hotkey =
        combo.isEmpty()
            ? QStringLiteral("释放热键没有设置，交还键盘只能连按两次 Esc。\n")
            : QStringLiteral("也可以按释放热键（%1）交还。\n").arg(combo);
    capture_->setToolTip(
        QStringLiteral("在远程画面里点一下即可开始输入键盘；连按两次 Esc 交还。\n") +
        hotkey +
        QStringLiteral("Alt+F4 与 Win 始终留在本机：Windows 在外壳层优先处理 Win 和\n"
                       "Alt+Tab，这些键常常还没到本程序就被本地吃掉了。"));
}

void SessionToolbar::set_release_key(const QKeySequence& key) {
    release_key_ = key;
    refresh_capture_tip();
}

void SessionToolbar::set_capture(bool captured) {
    capture_->setText(captured ? QStringLiteral("键盘 · 远程")
                               : QStringLiteral("键盘 · 本地"));
    theme::tint(capture_, captured ? theme::colors().accent : theme::colors().muted);
}

void SessionToolbar::set_zoomed(bool zoomed) {
    // The window decides the state as much as the button does, so the checked
    // flag is set rather than toggled here.
    fullscreen_button_->blockSignals(true);
    fullscreen_button_->setChecked(zoomed);
    fullscreen_button_->blockSignals(false);
    fullscreen_button_->setText(zoomed ? QStringLiteral("退出全屏")
                                       : QStringLiteral("全屏"));
    // The mark has to change with the words, or the button keeps promising the
    // opposite of what it now does.
    fullscreen_button_->setIcon(zoomed ? icons::restore() : icons::fullscreen());
}

void SessionToolbar::set_stats(int fps, int latency_ms) {
    if (!streaming_) {
        return;
    }
    // A negative latency means the reading is not available yet, which has to
    // look like an absence: a zero would read as a network nobody could build.
    // Zero itself is real but rounds down, so it says what it means.
    const QString rtt = latency_ms < 0
                            ? QStringLiteral("--")
                            : (latency_ms == 0
                                   ? QStringLiteral("<1ms")
                                   : QStringLiteral("%1ms").arg(latency_ms));
    stats_->setText(QStringLiteral("帧率 %1 · 延迟 %2").arg(fps).arg(rtt));
}

void SessionToolbar::set_quality_index(int index) {
    if (index >= 0 && index < quality_->count()) {
        quality_->setCurrentIndex(index);
    }
}

int SessionToolbar::quality_index() const { return quality_->currentIndex(); }
