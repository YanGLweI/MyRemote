#include "main_window.hpp"

#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>

#include <cstdlib>
#include <string>

#include "device_list.hpp"
#include "log.hpp"
#include "sessions_area.hpp"

namespace {

// A limited agent's SendInput is dropped by UIPI on elevated windows, which
// looks exactly like a dead session; flag those devices up front.
QString badge_text(const TunnelManager::DeviceInfo& info) {
    QString badges;
    if (info.flags & proto::kFlagServiceHost) {
        badges += QStringLiteral("〔服务〕");
    }
    if (info.flags & proto::kFlagLogonScreen) {
        badges += QStringLiteral("〔登录界面〕");
    } else if (info.elevation_known && !(info.flags & proto::kFlagConsoleOwner)) {
        badges += QStringLiteral("〔非控制台〕");
    }
    if (info.elevation_known && !info.elevated) {
        badges += QStringLiteral("〔受限〕");
    }
    return badges;
}

}  // namespace

MainWindow::MainWindow(const config::ServerConfig& cfg, QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("MyRemote Control Center"));
    resize(1100, 700);

    tunnels_ = std::make_unique<TunnelManager>(cfg.secret_key, cfg.max_connections);

    auto* central = new QWidget();
    setCentralWidget(central);
    auto* root_layout = new QHBoxLayout(central);
    root_layout->setContentsMargins(0, 0, 0, 0);

    auto* splitter = new QSplitter(Qt::Horizontal);
    root_layout->addWidget(splitter);

    auto* side_panel = new QWidget();
    auto* side_layout = new QVBoxLayout(side_panel);
    side_layout->setContentsMargins(0, 0, 0, 0);
    device_list_ = new DeviceListWidget();
    remark_button_ = new QPushButton(QStringLiteral("设置备注"));
    remark_button_->setEnabled(false);
    disconnect_button_ = new QPushButton(QStringLiteral("断开所选"));
    disconnect_button_->setToolTip(QStringLiteral(
        "关掉这些设备的会话标签并切断它们的隧道；对端会自动重连回来。"));
    side_layout->addWidget(device_list_, 1);
    side_layout->addWidget(remark_button_);
    side_layout->addWidget(disconnect_button_);

    sessions_ = new SessionsArea(*tunnels_);
    splitter->addWidget(side_panel);
    splitter->addWidget(sessions_);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({340, 760});

    statusBar()->showMessage(QStringLiteral("就绪"), 0);

    connect(device_list_, &DeviceListWidget::remote_control_requested, this,
            &MainWindow::on_control_requested);
    connect(device_list_, &DeviceListWidget::selection_changed, this,
            [this](QString id) { remark_button_->setEnabled(!id.isEmpty()); });
    connect(sessions_, &SessionsArea::note, this,
            [this](QString text) { statusBar()->showMessage(text, 8000); });
    connect(remark_button_, &QPushButton::clicked, this, [this]() {
        auto id = device_list_->selected_device_id();
        if (!id.has_value()) return;
        bool ok = false;
        QString cur = remarks_.value(QString::fromStdString(*id)).toString();
        QString text = QInputDialog::getText(this, QStringLiteral("设置备注"),
                                             QStringLiteral("这台设备的备注："),
                                             QLineEdit::Normal, cur, &ok);
        if (ok) {
            remarks_.setValue(QString::fromStdString(*id), text);
            on_refresh_remark(QString::fromStdString(*id));
        }
    });
    connect(disconnect_button_, &QPushButton::clicked, this, [this]() {
        for (const auto& id : device_list_->selected_device_ids()) {
            tunnels_->disconnect_device(id);
        }
    });
    connect(tunnels_.get(), &TunnelManager::device_registered, this,
            &MainWindow::on_device_registered);
    connect(tunnels_.get(), &TunnelManager::device_state_changed, this,
            &MainWindow::on_device_state_changed);

    if (!tunnels_->start(cfg.bind_address, cfg.listening_port)) {
        QMessageBox::critical(this, QStringLiteral("MyRemote"),
                              QStringLiteral("监听端口 %1 失败")
                                  .arg(cfg.listening_port));
        std::exit(1);
    }
}

MainWindow::~MainWindow() {
    // Tabs own their sessions, so this is also what stops every decoder.
    if (sessions_) {
        sessions_->close_all();
    }
    if (tunnels_) {
        tunnels_->stop();
    }
}

QString MainWindow::display_name(const TunnelManager::DeviceInfo& info) const {
    QString remark = remarks_.value(QString::fromStdString(info.device_id)).toString();
    QString name = remark.isEmpty() ? QString::fromStdString(info.device_name)
                                    : remark;
    return name + QStringLiteral(" ") + badge_text(info);
}

void MainWindow::publish_row(const TunnelManager::DeviceInfo& info) {
    const QString name = display_name(info);
    device_list_->upsert_device(info);
    sessions_->update_session(info, name);
}

void MainWindow::on_device_registered(QString device_id, QString, int, int) {
    TunnelManager::DeviceInfo info;
    if (!tunnels_->roster_for(device_id.toStdString(), &info)) {
        return;  // a rejected or not-yet-installed registration
    }
    publish_row(info);
}

void MainWindow::on_device_state_changed(QString device_id, int) {
    TunnelManager::DeviceInfo info;
    if (!tunnels_->roster_for(device_id.toStdString(), &info)) {
        return;
    }
    publish_row(info);
    if (info.state != DeviceState::Live) {
        // A service-managed host comes back in about two seconds; keep the
        // session armed rather than ending it.
        sessions_->suspend_session(info.device_id);
    }
}

void MainWindow::on_refresh_remark(QString device_id) {
    TunnelManager::DeviceInfo info;
    if (tunnels_->roster_for(device_id.toStdString(), &info)) {
        publish_row(info);
    }
}

void MainWindow::on_control_requested(QString device_id) {
    TunnelManager::DeviceInfo info;
    if (!tunnels_->roster_for(device_id.toStdString(), &info)) {
        statusBar()->showMessage(
            QStringLiteral("%1 现在没有可用的隧道").arg(device_id), 5000);
        return;
    }
    sessions_->open_session(info, display_name(info));
}
