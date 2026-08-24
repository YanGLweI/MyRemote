#pragma once

#include <QWidget>
#include <QListWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>

// Device list widget - displays all online clients in UI
class DeviceListWidget : public QWidget {
    Q_OBJECT
    
public:
    explicit DeviceListWidget(QWidget* parent = nullptr);
    ~DeviceListWidget() override;
    
    // Add or update device entry
    void add_device(const std::string& device_id, const std::string& device_name,
                   int width, int height, bool online, time_t connect_time);
    
    // Remove device from list
    void remove_device(const std::string& device_id);
    
    // Clear entire list
    void clear_devices();
    
    // Get currently selected device
    std::optional<std::string> get_selected_device_id();
    
signals:
    void remote_control_requested(const std::string& device_id);
    
private slots:
    void on_item_double_clicked(QListWidgetItem* item);
    
private:
    QListWidget* device_list_;
    QLabel* status_label_;
    QPushButton* remote_button_;
    
    void update_status_label(int count);
};
