#include "sessions_area.hpp"

#include <QLabel>
#include <QStackedWidget>
#include <QTabBar>
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
                       "关掉标签才会断开；键盘要先点一下画面才会送到对端。"));
    hint->setAlignment(Qt::AlignCenter);
    // Unwrapped, the longest line of this hint would set the minimum width of
    // the whole right pane for the life of the window.
    hint->setWordWrap(true);
    hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    hint->setMinimumWidth(0);
    hint->setStyleSheet(QStringLiteral("color: gray;"));
    box->addWidget(hint);
    return page;
}

}  // namespace

SessionsArea::SessionsArea(TunnelManager& tunnels, const QKeySequence& release_key,
                           QWidget* parent)
    : QWidget(parent),
      tunnels_(tunnels),
      release_key_(release_key),
      default_quality_(kQualityDefault) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    tabs_ = new QTabWidget();
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    // Each tab is a SessionView that owns its own decoder; the tab is the
    // session, so closing it is the only way to stop one.
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &SessionsArea::close_tab);
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) {
        refresh_zoom();
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

void SessionsArea::refresh_zoom() {
    SessionView* want = (zoom_wanted_ && tabs_->count())
                            ? qobject_cast<SessionView*>(tabs_->currentWidget())
                            : nullptr;
    if (want == zoomed_) {
        return;
    }
    if (zoomed_) {
        zoomed_->set_zoomed(false);
    }
    if (want) {
        want->set_zoomed(true);
    }
    zoomed_ = want;
    emit zoom_changed(want != nullptr);
}

void SessionsArea::close_tab(int index) {
    auto* page = qobject_cast<SessionView*>(tabs_->widget(index));
    if (page) {
        // The next tab opened starts where this one left off.
        default_quality_ = page->quality_index();
    }
    QWidget* widget = tabs_->widget(index);
    tabs_->removeTab(index);
    // Re-point the zoom before the page goes away, so closing the fullscreen
    // tab hands the window back instead of stranding it fullscreen.
    refresh_zoom();
    delete widget;
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

    auto* view =
        new SessionView(info.device_id, tunnels_, default_quality_, release_key_);
    connect(view, &SessionView::note, this, &SessionsArea::note);
    connect(view, &SessionView::zoom_requested, this, [this, view](bool on) {
        if (on) {
            zoom_wanted_ = true;
        } else if (tabs_->currentWidget() == view) {
            zoom_wanted_ = false;
        }
        refresh_zoom();
    });
    // The picture keeps focus after handing the keyboard back, which leaves
    // Tab nowhere to go; the tab bar is where the operator expects to land.
    connect(view, &SessionView::escape_released, this,
            [this] { tabs_->tabBar()->setFocus(); });
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
                     live ? QStringLiteral("已连接")
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
    zoom_wanted_ = false;
    while (tabs_->count() > 0) {
        close_tab(0);
    }
}
