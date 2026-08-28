#include "device_list.hpp"

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

void DeviceListWidget::upsert_device(const std::string& device_id,
                                    const std::string& device_name, int width,
                                    int height, const std::string& peer_ip,
                                    time_t connect_time) {
    QString text = QString("%1  [%2x%3]  %4\nid: %5  (connected %6)")
                       .arg(QString::fromStdString(device_name))
                       .arg(width)
                       .arg(height)
                       .arg(QString::fromStdString(peer_ip))
                       .arg(QString::fromStdString(device_id))
                       .arg(QDateTime::fromSecsSinceEpoch(
                                static_cast<qint64>(connect_time))
                                .toString("HH:mm:ss"));

    QListWidgetItem* item = find_by_id(device_id);
    if (!item) {
        item = new QListWidgetItem();
        item->setData(Qt::UserRole, QString::fromStdString(device_id));
        item->setBackground(QColor(240, 255, 240));
        device_list_->addItem(item);
    }
    item->setText(text);

    int count = device_list_->count();
    status_label_->setText(QString("%1 device(s) online").arg(count));
    status_label_->setStyleSheet(count > 0 ? "color: #2E8B57; font-weight: bold;"
                                           : "color: gray; font-style: italic;");
}

void DeviceListWidget::remove_device(const std::string& device_id) {
    QListWidgetItem* item = find_by_id(device_id);
    if (item) {
        device_list_->takeItem(device_list_->row(item));
        delete item;
    }
    int count = device_list_->count();
    status_label_->setText(count > 0 ? QString("%1 device(s) online").arg(count)
                                     : "No devices online");
    status_label_->setStyleSheet(count > 0 ? "color: #2E8B57; font-weight: bold;"
                                           : "color: gray; font-style: italic;");
}

void DeviceListWidget::clear_devices() {
    device_list_->clear();
    status_label_->setText("No devices online");
    status_label_->setStyleSheet("color: gray; font-style: italic;");
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
