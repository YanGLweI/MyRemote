#include "device_list.hpp"

#include <QBrush>
#include <QDateTime>
#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

DeviceListWidget::DeviceListWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    device_list_ = new QListWidget();
    device_list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(device_list_, &QListWidget::itemDoubleClicked, this,
            &DeviceListWidget::on_item_double_clicked);
    connect(device_list_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* cur, QListWidgetItem*) {
                emit selection_changed(cur ? cur->data(Qt::UserRole).toString()
                                           : QString());
            });
    layout->addWidget(device_list_);

    status_label_ = new QLabel("No devices online");
    status_label_->setAlignment(Qt::AlignCenter);
    status_label_->setStyleSheet("color: gray; font-style: italic;");
    layout->addWidget(status_label_);
}

QListWidgetItem* DeviceListWidget::find_by_id(const std::string& device_id) const {
    for (int i = 0; i < device_list_->count(); ++i) {
        QListWidgetItem* item = device_list_->item(i);
        if (item->data(Qt::UserRole).toString().toStdString() == device_id) {
            return item;
        }
    }
    return nullptr;
}

namespace {

QString state_marker(DeviceState state) {
    switch (state) {
        case DeviceState::Live: return QStringLiteral("●");
        case DeviceState::Reconnecting: return QStringLiteral("◌");
        case DeviceState::Offline: return QStringLiteral("○");
    }
    return QStringLiteral("?");
}

QString state_note(const TunnelManager::DeviceInfo& info) {
    const QString stamp = QDateTime::fromSecsSinceEpoch(
                              static_cast<qint64>(info.state == DeviceState::Live
                                                      ? info.connect_time
                                                      : info.last_seen_time))
                              .toString("HH:mm:ss");
    switch (info.state) {
        case DeviceState::Live:
            return QStringLiteral("已连接 %1").arg(stamp);
        case DeviceState::Reconnecting:
            return QStringLiteral("重连中 · 上次 %1").arg(stamp);
        case DeviceState::Offline:
            return QStringLiteral("离线 · 上次 %1").arg(stamp);
    }
    return QString();
}

}  // namespace

void DeviceListWidget::upsert_device(const TunnelManager::DeviceInfo& info) {
    const std::string device_id = info.device_id;
    QString text =
        QString("%1  [%2x%3]  %4\n%5  id: %6  ·  %7")
            .arg(state_marker(info.state))
            .arg(QString::fromStdString(info.device_name))
            .arg(info.screen_width)
            .arg(info.screen_height)
            .arg(QString::fromStdString(info.peer_ip))
            .arg(QString::fromStdString(device_id))
            .arg(state_note(info));

    QListWidgetItem* item = find_by_id(device_id);
    if (!item) {
        item = new QListWidgetItem();
        item->setData(Qt::UserRole, QString::fromStdString(device_id));
        device_list_->addItem(item);
    }
    item->setText(text);
    item->setData(StateRole, static_cast<int>(info.state));
    switch (info.state) {
        case DeviceState::Live:
            item->setForeground(QBrush());  // palette default
            break;
        case DeviceState::Reconnecting:
            item->setForeground(QColor(176, 122, 24));
            break;
        case DeviceState::Offline:
            item->setForeground(QColor(140, 140, 140));
            break;
    }

    int live = 0;
    for (int i = 0; i < device_list_->count(); ++i) {
        if (device_list_->item(i)->data(StateRole).toInt() ==
            static_cast<int>(DeviceState::Live)) {
            ++live;
        }
    }
    const int rows = device_list_->count();
    status_label_->setText(live > 0
                               ? QString("%1/%2 device(s) online")
                                     .arg(live)
                                     .arg(rows)
                               : QStringLiteral("No devices online"));
    status_label_->setStyleSheet(live > 0 ? "color: #2E8B57; font-weight: bold;"
                                         : "color: gray; font-style: italic;");
}

std::optional<std::string> DeviceListWidget::selected_device_id() const {
    QListWidgetItem* current = device_list_->currentItem();
    if (current) {
        return current->data(Qt::UserRole).toString().toStdString();
    }
    return std::nullopt;
}

void DeviceListWidget::on_item_double_clicked(QListWidgetItem* item) {
    emit remote_control_requested(item->data(Qt::UserRole).toString());
}

std::vector<std::string> DeviceListWidget::selected_device_ids() const {
    std::vector<std::string> ids;
    for (QListWidgetItem* item : device_list_->selectedItems()) {
        ids.push_back(item->data(Qt::UserRole).toString().toStdString());
    }
    return ids;
}
