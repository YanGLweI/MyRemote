#include "device_panel.hpp"

#include <QDateTime>
#include <QEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QSortFilterProxyModel>
#include <QVBoxLayout>

#include "device_list_model.hpp"
#include "device_row_delegate.hpp"

namespace {

// The search box matches anything the operator might remember about a machine,
// not just the name shown on the row.
class RosterFilter : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
    bool filterAcceptsRow(int source_row, const QModelIndex&) const override {
        const QString needle = filterRegularExpression().pattern().toLower();
        if (needle.isEmpty()) {
            return true;
        }
        const QModelIndex row = sourceModel()->index(source_row, 0);
        const QStringList haystack{
            row.data(DeviceListModel::NameRole).toString(),
            row.data(DeviceListModel::SidRole).toString(),
            row.data(DeviceListModel::MetaRole).toString(),
        };
        for (const QString& field : haystack) {
            if (field.toLower().contains(needle)) {
                return true;
            }
        }
        return false;
    }
};

}  // namespace

DevicePanel::DevicePanel(QWidget* parent) : QWidget(parent) {
    auto* box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(6);

    search_ = new QLineEdit();
    search_->setPlaceholderText(QStringLiteral("搜索名称 / IP / 设备 id / 备注"));
    search_->setClearButtonEnabled(true);

    model_ = new DeviceListModel(this);
    filter_ = new RosterFilter(this);
    filter_->setSourceModel(model_);

    list_ = new QListView();
    list_->setModel(filter_);
    list_->setItemDelegate(new DeviceRowDelegate(list_));
    list_->setSelectionBehavior(QAbstractItemView::SelectRows);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setUniformItemSizes(true);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setMouseTracking(true);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);

    footer_ = new QLabel(QStringLiteral("还没有设备注册"));
    footer_->setContentsMargins(2, 0, 2, 0);
    // The count changes width as devices come and go; that must not become the
    // panel's minimum width, or the roster resizes itself under the splitter.
    footer_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    footer_->setMinimumWidth(0);

    box->addWidget(search_);
    box->addWidget(list_, 1);
    box->addWidget(footer_);

    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        filter_->setFilterFixedString(text);
        refresh_footer();
    });
    connect(list_, &QListView::doubleClicked, this, [this](const QModelIndex& index) {
        emit remote_control_requested(index.data(DeviceListModel::SidRole)
                                          .toString());
    });
    connect(list_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) {
                const auto ids = selected_device_ids();
                emit selection_changed(ids.empty()
                                           ? QString()
                                           : QString::fromStdString(ids.front()));
            });
    connect(list_, &QListView::customContextMenuRequested, this,
            &DevicePanel::show_context_menu);
    list_->viewport()->installEventFilter(this);
}

bool DevicePanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == list_->viewport() && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Enter || key->key() == Qt::Key_Return) {
            activate_current();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void DevicePanel::activate_current() {
    const QModelIndex current = list_->currentIndex();
    if (current.isValid()) {
        emit remote_control_requested(current.data(DeviceListModel::SidRole)
                                          .toString());
    }
}

void DevicePanel::upsert(const TunnelManager::DeviceInfo& info,
                         const QString& remark) {
    DeviceListModel::Entry entry;
    entry.sid = QString::fromStdString(info.device_id);
    entry.host = QString::fromStdString(info.device_name);
    entry.remark = remark;
    entry.name = remark.isEmpty() ? entry.host : remark;
    entry.ip = QString::fromStdString(info.peer_ip);
    entry.width = info.screen_width;
    entry.height = info.screen_height;
    entry.state = info.state;
    entry.connect_time = info.connect_time;
    entry.last_seen_time = info.last_seen_time;
    entry.flags = info.flags;
    entry.elevated = info.elevated;
    entry.elevation_known = info.elevation_known;
    model_->upsert(entry);
    refresh_footer();
}

void DevicePanel::refresh_footer() {
    const int total = model_->rowCount();
    const int shown = filter_->rowCount();
    const int live = model_->live_count();
    QString text;
    if (total == 0) {
        text = QStringLiteral("还没有设备注册");
    } else if (shown == 0) {
        text = QStringLiteral("没有匹配的设备");
    } else if (shown != total) {
        text = QStringLiteral("%1 / %2 台 · %3 在线")
                   .arg(shown)
                   .arg(total)
                   .arg(live);
    } else {
        text = QStringLiteral("%1 台 · %2 在线").arg(total).arg(live);
    }
    footer_->setText(text);
}

std::optional<std::string> DevicePanel::selected_device_id() const {
    const QModelIndex current = list_->currentIndex();
    if (!current.isValid()) {
        return std::nullopt;
    }
    return current.data(DeviceListModel::SidRole).toString().toStdString();
}

std::vector<std::string> DevicePanel::selected_device_ids() const {
    std::vector<std::string> ids;
    const QList<QModelIndex> selected = list_->selectionModel()->selectedRows();
    for (const QModelIndex& index : selected) {
        ids.push_back(index.data(DeviceListModel::SidRole).toString().toStdString());
    }
    return ids;
}

QString DevicePanel::detail_text(const QString& device_id) const {
    const auto entry = model_->entry(device_id);
    if (!entry) {
        return QString();
    }
    const QString stamp = entry->state == DeviceState::Live
                              ? QDateTime::fromSecsSinceEpoch(
                                    static_cast<qint64>(entry->connect_time))
                                    .toString("yyyy-MM-dd HH:mm:ss")
                              : QStringLiteral("未记录");
    QString caps;
    if (entry->flags & proto::kFlagServiceHost) {
        caps += QStringLiteral("服务宿主 ");
    }
    if (entry->flags & proto::kFlagIsSystem) {
        caps += QStringLiteral("SYSTEM 账户 ");
    }
    if (entry->flags & proto::kFlagSecureDesktop) {
        caps += QStringLiteral("可跟随登录界面 ");
    }
    if (entry->flags & proto::kFlagConsoleOwner) {
        caps += QStringLiteral("持有控制台 ");
    }
    return QStringLiteral("名称：%1\n主机名：%2\n备注：%3\n设备 id：%4\n"
                          "地址：%5\n分辨率：%6x%7\n状态：%8\n本次上线：%9\n"
                          "权限：%10\n能力：%11")
        .arg(entry->name, entry->host,
             entry->remark.isEmpty() ? QStringLiteral("（无）") : entry->remark,
             entry->sid, entry->ip)
        .arg(entry->width)
        .arg(entry->height)
        .arg(DeviceListModel::state_word(entry->state), stamp,
             !entry->elevation_known
                 ? QStringLiteral("未知（旧版 agent）")
                 : (entry->elevated ? QStringLiteral("已提权")
                                    : QStringLiteral("受限")),
             caps.trimmed().isEmpty() ? QStringLiteral("（无）") : caps.trimmed());
}

void DevicePanel::show_context_menu(const QPoint& view_pos) {
    const QModelIndex index = list_->indexAt(view_pos);
    if (!index.isValid()) {
        return;  // empty space: nothing to act on, and no menu to guess at
    }
    const QString sid = index.data(DeviceListModel::SidRole).toString();
    if (!list_->selectionModel()->isSelected(index)) {
        list_->clearSelection();
        list_->setCurrentIndex(index);
        list_->selectionModel()->select(
            index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    QStringList targets;
    for (const std::string& id : selected_device_ids()) {
        targets << QString::fromStdString(id);
    }
    if (targets.isEmpty()) {
        targets << sid;  // a right-click on an unselected row acts on that row
    }

    QMenu menu(this);
    QAction* control = menu.addAction(QStringLiteral("远程控制"));
    QAction* remark = menu.addAction(QStringLiteral("设置备注"));
    menu.addSeparator();
    QAction* disconnect = menu.addAction(targets.size() > 1
                                             ? QStringLiteral("断开这 %1 台")
                                                   .arg(targets.size())
                                             : QStringLiteral("断开连接"));
    menu.addSeparator();
    QAction* properties = menu.addAction(QStringLiteral("属性"));

    QAction* chosen = menu.exec(list_->viewport()->mapToGlobal(view_pos));
    if (!chosen) {
        return;
    }
    if (chosen == control) {
        emit remote_control_requested(sid);
    } else if (chosen == remark) {
        emit remark_requested(sid);
    } else if (chosen == disconnect) {
        emit disconnect_requested(targets);
    } else if (chosen == properties) {
        QMessageBox::information(this, QStringLiteral("设备属性"),
                                 detail_text(sid));
    }
}
