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

    title_ = new QLabel(QStringLiteral("Remote session"));
    QFont bold = title_->font();
    bold.setBold(true);
    title_->setFont(bold);
    detail_ = new QLabel();
    state_ = new QLabel();
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

    row->addWidget(title_);
    row->addWidget(detail_);
    row->addWidget(state_);
    row->addStretch(1);
    row->addWidget(fps_);
    row->addWidget(quality_);
    row->addWidget(logon_button_);
    row->addWidget(stop_button_);

    connect(quality_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int idx) { emit quality_selected(idx); });
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
    if (!on) {
        fps_->setText(QStringLiteral("NET -- | DEC --"));
    }
}

void SessionToolbar::set_supports_logon(bool on) {
    logon_button_->setEnabled(on && streaming_);
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
