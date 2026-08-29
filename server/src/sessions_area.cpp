#include "sessions_area.hpp"

#include <QLabel>
#include <QStackedWidget>
#include <QTabWidget>
#include <QVBoxLayout>

#include "log.hpp"
#include "session_view.hpp"

namespace {

constexpr int kMaxConcurrentSessions = 4;

QWidget* make_empty_page() {
    auto* page = new QWidget();
    auto* box = new QVBoxLayout(page);
    auto* hint = new QLabel(
        QStringLiteral("双击左侧设备开始远程控制\n\n"
                       "同一个设备的标签会一直保持连接，关掉标签才会断开。"));
    hint->setAlignment(Qt::AlignCenter);
    hint->setStyleSheet(QStringLiteral("color: gray;"));
    box->addWidget(hint);
    return page;
}

}  // namespace

SessionsArea::SessionsArea(TunnelManager& tunnels, QWidget* parent)
    : QWidget(parent), tunnels_(tunnels), default_quality_(kQualityDefault) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    tabs_ = new QTabWidget();
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    // Each tab is a SessionView that owns its own decoder; the tab is the
    // session, so closing it is the only way to stop one.
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
        QWidget* page = tabs_->widget(index);
        tabs_->removeTab(index);
        delete page;
    });

    auto* stack = new QStackedWidget();
    stack->addWidget(make_empty_page());
    stack->addWidget(tabs_);
    root->addWidget(stack);

    // The empty page and the tab strip cannot both be useful at once.
    connect(tabs_, &QTabWidget::currentChanged, this, [this, stack](int) {
        stack->setCurrentIndex(tabs_->count() ? 1 : 0);
    });
}

int SessionsArea::tab_of(const std::string& device_id) const {
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* view = qobject_cast<SessionView*>(tabs_->widget(i));
        if (view && view->device_id() == device_id) {
            return i;
        }
    }
    return -1;
}

void SessionsArea::open_session(const TunnelManager::DeviceInfo& info,
                                const QString& display_name) {
    const int existing = tab_of(info.device_id);
    if (existing >= 0) {
        tabs_->setCurrentIndex(existing);
        return;
    }
    if (tabs_->count() >= kMaxConcurrentSessions) {
        emit note(QStringLiteral("最多同时保持 %1 路会话，请先关掉一个标签")
                      .arg(kMaxConcurrentSessions));
        return;
    }

    auto* view = new SessionView(info.device_id, tunnels_, default_quality_);
    connect(view, &SessionView::note, this, &SessionsArea::note);
    const int index = tabs_->addTab(view, display_name);
    tabs_->setCurrentIndex(index);
    view->set_header(display_name, QStringLiteral("%1 · %2x%3")
                                        .arg(QString::fromStdString(info.peer_ip))
                                        .arg(info.screen_width)
                                        .arg(info.screen_height),
                     QStringLiteral("连接中"), true);
    view->begin();
}

void SessionsArea::update_session(const TunnelManager::DeviceInfo& info,
                                 const QString& display_name) {
    const int index = tab_of(info.device_id);
    if (index < 0) {
        return;
    }
    auto* view = qobject_cast<SessionView*>(tabs_->widget(index));
    if (!view) {
        return;
    }
    tabs_->setTabText(index, display_name);
    const bool live = info.state == DeviceState::Live;
    view->set_header(display_name,
                     QStringLiteral("%1 · %2x%3")
                         .arg(QString::fromStdString(info.peer_ip))
                         .arg(info.screen_width)
                         .arg(info.screen_height),
                     live ? QStringLiteral("")
                          : (info.state == DeviceState::Reconnecting
                                 ? QStringLiteral("重连中")
                                 : QStringLiteral("离线")),
                     live);
}

void SessionsArea::suspend_session(const std::string& device_id) {
    const int index = tab_of(device_id);
    if (index < 0) {
        return;
    }
    if (auto* view = qobject_cast<SessionView*>(tabs_->widget(index))) {
        view->suspend();
    }
}

int SessionsArea::session_count() const { return tabs_->count(); }

void SessionsArea::close_all() {
    while (tabs_->count() > 0) {
        QWidget* page = tabs_->widget(0);
        tabs_->removeTab(0);
        delete page;
    }
}
