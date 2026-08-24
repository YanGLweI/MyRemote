#include "device_list.hpp"
#include <QHeaderView>
#include <QDateTime>

DeviceListWidget::DeviceListWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    
    // Create device list widget
    device_list_ = new QListWidget();
    device_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    device_list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(device_list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0) {
            QListWidgetItem* item = device_list_->item(row);
            QString device_id = item->data(Qt::UserRole).toString();
            emit remote_control_requested(device_id.toStdString());
        }
    });
    
    // Double-click handler for quick remote control
    connect(device_list_, &QListWidget::itemDoubleClicked, this, &DeviceListWidget::on_item_double_clicked);
    
    layout->addWidget(device_list_);
    
    // Status label
    status_label_ = new QLabel("No devices online");
    status_label_->setAlignment(Qt::AlignCenter);
    status_label_->setStyleSheet("color: gray; font-style: italic;");
    layout->addWidget(status_label_);
    
    update_status_label(0);
}

DeviceListWidget::~DeviceListWidget() {}

void DeviceListWidget::add_device(const std::string& device_id, const std::string& device_name,
                                   int width, int height, bool online, time_t connect_time) {
    // Find existing item or create new one
    QList<QListWidgetItem*> items = device_list_->findItems(QString::fromStdString(device_id), 
                                                            Qt::MatchExactly);
    
    QListWidgetItem* item = nullptr;
    if (!items.isEmpty()) {
        item = items.first();
        
        // Update existing entry
        QString status_str = online ? "● Online" : "○ Offline";
        item->setText(QString("%1 (%2x%3) %4")
            .arg(QString::fromStdString(device_name))
            .arg(width)
            .arg(height)
            .arg(status_str));
            
        // Store offline timestamp
        if (!online) {
            QDateTime dt = QDateTime::fromTime_t(connect_time);
            item->setData(Qt::UserRole + 1, dt.toString("yyyy-MM-dd HH:mm:ss"));
        }
    } else {
        // Create new item
        item = new QListWidgetItem();
        
        QString info_str;
        if (online) {
            QDateTime dt = QDateTime::fromTime_t(connect_time);
            info_str = QString("%1 (%2x%3) Connected: %4")
                .arg(QString::fromStdString(device_name))
                .arg(width)
                .arg(height)
                .arg(dt.toString("HH:mm:ss"));
                
            item->setBackground(QColor(240, 255, 240));  // Light green background
        } else {
            info_str = QString("%1 (%2x%3) Disconnected at %4")
                .arg(QString::fromStdString(device_name))
                .arg(width)
                .arg(height)
                .arg(QDateTime::fromTime_t(connect_time).toString("yyyy-MM-dd HH:mm:ss"));
                
            item->setBackground(QColor(255, 240, 240));  // Light red background
        }
        
        item->setText(info_str);
        item->setData(Qt::UserRole, QString::fromStdString(device_id));
        
        device_list_->addItem(item);
        
        // Select first device if it's the only one online
        if (online && device_list_->count() == 1) {
            device_list_->setCurrentItem(item);
        }
    }
}

void DeviceListWidget::remove_device(const std::string& device_id) {
    QList<QListWidgetItem*> items = device_list_->findItems(QString::fromStdString(device_id), 
                                                            Qt::MatchExactly);
    
    if (!items.isEmpty()) {
        QListWidgetItem* item = items.first();
        device_list_->takeItem(device_list_->row(item));
        delete item;
        
        update_status_label(device_list_->count());
    }
}

void DeviceListWidget::clear_devices() {
    device_list_->clear();
    update_status_label(0);
}

std::optional<std::string> DeviceListWidget::get_selected_device_id() {
    QListWidgetItem* current = device_list_->currentItem();
    if (current) {
        return current->data(Qt::UserRole).toString().toStdString();
    }
    return std::nullopt;
}

void DeviceListWidget::update_status_label(int count) {
    if (count > 0) {
        status_label_->setText(QString("%1 device(s) online").arg(count));
        status_label_->setStyleSheet("color: #2E8B57; font-weight: bold;");
    } else {
        status_label_->setText("No devices online");
        status_label_->setStyleSheet("color: gray; font-style: italic;");
    }
}

void DeviceListWidget::on_item_double_clicked(QListWidgetItem* item) {
    // Trigger remote control when double-clicking
    QString device_id = item->data(Qt::UserRole).toString();
    emit remote_control_requested(device_id.toStdString());
}
