#include "main_window.hpp"

#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <cstdlib>
#include <string>

#include "device_list_model.hpp"
#include "device_panel.hpp"
#include "log.hpp"
#include "log_drawer.hpp"
#include "log_tail.hpp"
#include "sessions_area.hpp"

MainWindow::MainWindow(const config::ServerConfig& cfg, LogTail& log_tail,
                       QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("MyRemote 控制中心"));
    resize(1100, 700);

    tunnels_ = std::make_unique<TunnelManager>(cfg.secret_key, cfg.max_connections);

    auto* central = new QWidget();
    setCentralWidget(central);
    auto* root_layout = new QVBoxLayout(central);
    root_layout->setContentsMargins(0, 0, 0, 0);

    // The roster and the sessions sit above the log rather than beside it: a
    // stack trace read next to the picture it explains is the point.
    stack_ = new QSplitter(Qt::Vertical);
    root_layout->addWidget(stack_);

    auto* splitter = new QSplitter(Qt::Horizontal);
    stack_->addWidget(splitter);

    side_panel_ = new QWidget();
    auto* side_layout = new QVBoxLayout(side_panel_);
    side_layout->setContentsMargins(8, 8, 8, 8);
    side_layout->setSpacing(6);
    device_list_ = new DevicePanel();
    remark_button_ = new QPushButton(QStringLiteral("设置备注"));
    remark_button_->setEnabled(false);
    disconnect_button_ = new QPushButton(QStringLiteral("断开所选"));
    disconnect_button_->setEnabled(false);
    disconnect_button_->setToolTip(QStringLiteral(
        "切断这些设备的隧道；对端会自动重连回来。\n"
        "要结束会话标签，请关掉标签本身。"));
    side_layout->addWidget(device_list_, 1);
    side_layout->addWidget(remark_button_);
    side_layout->addWidget(disconnect_button_);

    const QString configured_release =
        settings_.value(QStringLiteral("input/release_key")).toString();
    if (!configured_release.isEmpty()) {
        release_key_ = QKeySequence(configured_release);
    }
    sessions_ = new SessionsArea(*tunnels_, release_key_);
    splitter->addWidget(side_panel_);
    splitter->addWidget(sessions_);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    // Now that nothing on the right forces a wide window any more, the divider
    // could be dragged until the roster was unreadable; keep it usable instead.
    side_panel_->setMinimumWidth(260);
    splitter->setCollapsible(0, false);
    splitter->setSizes({340, 760});

    log_drawer_ = new LogDrawer(log_tail);
    log_drawer_->setMinimumHeight(120);
    log_drawer_->setVisible(false);
    stack_->addWidget(log_drawer_);
    stack_->setStretchFactor(0, 1);
    stack_->setStretchFactor(1, 0);
    stack_->setSizes({600, 220});

    // A QToolButton, not a QPushButton: the push button's own margins are
    // ~20px taller than the status bar and would raise the window's minimum
    // height for an exit that is only ever a word to click.
    log_button_ = new QToolButton();
    log_button_->setText(QStringLiteral("事件日志"));
    log_button_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    log_button_->setCheckable(true);
    log_button_->setAutoRaise(true);
    log_button_->setToolTip(QStringLiteral(
        "这台控制中心自己的运行记录：注册、隧道、解码、以及会话为什么没了。\n"
        "名字后面跟着的数字是警告和错误的条数。完整记录在日志文件里。"));
    // The button's checked state is the only thing that decides visibility, so
    // anything that wants the log gone (fullscreen) unchecks it rather than
    // hiding the pane behind its back.
    connect(log_button_, &QToolButton::toggled, this,
            [this](bool on) { log_drawer_->setVisible(on); });
    connect(log_drawer_, &LogDrawer::problem_count_changed, this, [this](int count) {
        log_button_->setText(count > 0 ? QStringLiteral("事件日志 · %1").arg(count)
                                       : QStringLiteral("事件日志"));
    });
    statusBar()->addPermanentWidget(log_button_);

    statusBar()->showMessage(QStringLiteral("就绪"), 0);

    connect(device_list_, &DevicePanel::remote_control_requested, this,
            &MainWindow::on_control_requested);
    connect(device_list_, &DevicePanel::remark_requested, this,
            &MainWindow::prompt_remark);
    connect(device_list_, &DevicePanel::disconnect_requested, this,
            [this](QStringList ids) {
                for (const QString& id : ids) {
                    tunnels_->disconnect_device(id.toStdString());
                }
            });
    connect(device_list_, &DevicePanel::selection_changed, this, [this](QString id) {
        remark_button_->setEnabled(!id.isEmpty());
        disconnect_button_->setEnabled(!device_list_->selected_device_ids().empty());
    });
    connect(sessions_, &SessionsArea::note, this,
            [this](QString text) { statusBar()->showMessage(text, 8000); });
    connect(sessions_, &SessionsArea::zoom_changed, this,
            &MainWindow::on_zoom_changed);
    connect(remark_button_, &QPushButton::clicked, this, [this]() {
        auto id = device_list_->selected_device_id();
        if (id.has_value()) {
            prompt_remark(QString::fromStdString(*id));
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

QString MainWindow::remark_of(const std::string& device_id) const {
    return settings_.value(QString::fromStdString(device_id)).toString();
}

QString MainWindow::display_name(const TunnelManager::DeviceInfo& info) const {
    return DeviceListModel::display_name(info, remark_of(info.device_id));
}

void MainWindow::publish_row(const TunnelManager::DeviceInfo& info) {
    device_list_->upsert(info, remark_of(info.device_id));
    sessions_->update_session(info, display_name(info));
}

void MainWindow::prompt_remark(const QString& device_id) {
    bool ok = false;
    const QString current = settings_.value(device_id).toString();
    const QString text =
        QInputDialog::getText(this, QStringLiteral("设置备注"),
                              QStringLiteral("这台设备的备注："), QLineEdit::Normal,
                              current, &ok);
    if (!ok) {
        return;
    }
    settings_.setValue(device_id, text);
    on_refresh_remark(device_id);
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

void MainWindow::on_zoom_changed(bool on) {
    if (zoomed_ == on) {
        return;  // the zoom moving between tabs must not lose the window geom
    }
    zoomed_ = on;
    // Deliberately keeps the tab strip and the session toolbar on screen: the
    // old fullscreen hid the only buttons that could leave it.
    side_panel_->setVisible(!on);
    if (on) {
        log_button_->setChecked(false);  // the drawer's height belongs to the picture
        showFullScreen();
    } else {
        showNormal();
    }
}
