#pragma once

#include <QWidget>

#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include "tunnel_manager.hpp"

class QLabel;
class QListWidget;
class QListWidgetItem;

// Roster of every device seen this run. Rows survive their tunnel: a device
// that is reconnecting must still be on screen, or the console looks broken.
class DeviceListWidget : public QWidget {
    Q_OBJECT

public:
    explicit DeviceListWidget(QWidget* parent = nullptr);

    enum ItemDataRole { StateRole = Qt::UserRole + 1 };

    void upsert_device(const TunnelManager::DeviceInfo& info);

    std::optional<std::string> selected_device_id() const;
    std::vector<std::string> selected_device_ids() const;

signals:
    void remote_control_requested(QString device_id);
    void selection_changed(QString device_id);

private slots:
    void on_item_double_clicked(QListWidgetItem* item);

private:
    QListWidgetItem* find_by_id(const std::string& device_id) const;

    QListWidget* device_list_;
    QLabel* status_label_;
};
