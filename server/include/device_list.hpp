#pragma once

#include <QWidget>

#include <ctime>
#include <optional>
#include <string>

class QLabel;
class QListWidget;
class QListWidgetItem;

// Live roster of registered clients.
class DeviceListWidget : public QWidget {
    Q_OBJECT

public:
    explicit DeviceListWidget(QWidget* parent = nullptr);

    void upsert_device(const std::string& device_id, const std::string& device_name,
                       int width, int height, const std::string& peer_ip,
                       time_t connect_time);
    void remove_device(const std::string& device_id);
    void clear_devices();

    std::optional<std::string> selected_device_id() const;

signals:
    void remote_control_requested(QString device_id);

private slots:
    void on_item_double_clicked(QListWidgetItem* item);

private:
    QListWidgetItem* find_by_id(const std::string& device_id) const;

    QListWidget* device_list_;
    QLabel* status_label_;
};
