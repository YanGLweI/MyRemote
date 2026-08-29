#include "session_toolbar.hpp"

#include <QComboBox>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

#include "remote_controller.hpp"

SessionToolbar::SessionToolbar(QWidget* parent) : QWidget(parent) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(8, 4, 8, 4);
    row->setSpacing(8);

    title_ = new QLabel(QStringLiteral("会话"));
    QFont bold = title_->font();
    bold.setBold(true);
    title_->setFont(bold);
    detail_ = new QLabel();
    state_ = new QLabel();
    capture_ = new QLabel();
    capture_->setText(QStringLiteral("  键盘 · 本地"));
    capture_->setToolTip(QStringLiteral(
        "在远程画面里点一下即可开始输入键盘；连按两次 Esc 交还。\n"
        "也可以按释放热键（默认 Ctrl+Alt+Shift+R）交还。\n"
        "Alt+F4 与 Win 始终留在本机：Windows 在外壳层优先处理 Win 和\n"
        "Alt+Tab，这些键常常还没到本程序就被本地吃掉了。"));
    fps_ = new QLabel(QStringLiteral("NET -- | DEC --"));

    quality_ = new QComboBox();
    for (int i = 0; i < kQualityPresetCount; ++i) {
        quality_->addItem(QString::fromUtf8(kQualityPresets[i].label));
    }
    quality_->setCurrentIndex(kQualityDefault);
    quality_->setToolTip(QStringLiteral("画质档位（只作用于这台设备）"));

    logon_button_ = new QPushButton(QStringLiteral("返回登录界面"));
    logon_button_->setEnabled(false);
    logon_button_->setToolTip(QStringLiteral(
        "锁定工作站，让对端回到登录界面后可以远程输入密码登录。\n"
        "Ctrl+Alt+Del 无法被注入：若策略要求按 Ctrl+Alt+Del 才开始登录，\n"
        "请将该机器的 DisableCAD 设为 1。"));

    stop_button_ = new QPushButton(QStringLiteral("断开"));

    fullscreen_button_ = new QPushButton(QStringLiteral("全屏"));
    fullscreen_button_->setCheckable(true);
    fullscreen_button_->setToolTip(QStringLiteral(
        "让这台设备铺满整个窗口。标签栏和这一条工具栏都会留在画面上方，\n"
        "出口一直在屏幕上。"));

    row->addWidget(title_);
    row->addWidget(detail_);
    row->addWidget(state_);
    row->addWidget(capture_);
    row->addStretch(1);
    row->addWidget(fps_);
    row->addWidget(quality_);
    row->addWidget(logon_button_);
    row->addWidget(fullscreen_button_);
    row->addWidget(stop_button_);

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
    detail_->setText(detail.isEmpty() ? QString() : QStringLiteral("  %1").arg(detail));
}

void SessionToolbar::set_state_text(const QString& text, bool live) {
    state_->setText(text.isEmpty() ? QString() : QStringLiteral("  %1").arg(text));
    state_->setStyleSheet(live ? QStringLiteral("color: #2E8B57;")
                               : QStringLiteral("color: #B07A18;"));
}

void SessionToolbar::set_streaming(bool on) {
    streaming_ = on;
    // One button, two honest labels: what it says is what it does.
    stop_button_->setText(on ? QStringLiteral("断开")
                             : QStringLiteral("重新连接"));
    capture_->setVisible(on);
    if (!on) {
        fps_->setText(QStringLiteral("NET -- | DEC --"));
    }
}

void SessionToolbar::set_supports_logon(bool on) {
    logon_button_->setEnabled(on && streaming_);
}

void SessionToolbar::set_capture(bool captured) {
    capture_->setText(captured ? QStringLiteral("  键盘 · 远程")
                              : QStringLiteral("  键盘 · 本地"));
    capture_->setStyleSheet(captured ? QStringLiteral("color: #3E9B6E;")
                                     : QStringLiteral("color: #98A2AD;"));
}

void SessionToolbar::set_zoomed(bool zoomed) {
    // The window decides the state as much as the button does, so the checked
    // flag is set rather than toggled here.
    fullscreen_button_->blockSignals(true);
    fullscreen_button_->setChecked(zoomed);
    fullscreen_button_->blockSignals(false);
    fullscreen_button_->setText(zoomed ? QStringLiteral("退出全屏")
                                       : QStringLiteral("全屏"));
}

void SessionToolbar::set_fps(int net_fps, int decoded_fps) {
    if (!streaming_) {
        return;
    }
    fps_->setText(QStringLiteral("NET %1 | DEC %2").arg(net_fps).arg(decoded_fps));
}

void SessionToolbar::set_quality_index(int index) {
    if (index >= 0 && index < quality_->count()) {
        quality_->setCurrentIndex(index);
    }
}

int SessionToolbar::quality_index() const { return quality_->currentIndex(); }
