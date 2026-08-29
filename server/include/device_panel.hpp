#pragma once

#include <QWidget>

#include <optional>
#include <string>
#include <vector>

#include "tunnel_manager.hpp"

class DeviceListModel;
class QListView;
class QLineEdit;
class QLabel;
class QSortFilterProxyModel;

// The roster as the operator meets it: a search box, one painted row per
// machine seen this run, and a context menu so the common actions do not need
// a button each. Rows are never removed during a run; a device that is
// reconnecting must stay on screen or the console reads as broken.
class DevicePanel : public QWidget {
    Q_OBJECT

public:
    explicit DevicePanel(QWidget* parent = nullptr);

    // One roster record in, one row out. The remark comes from the window's
    // settings because it is the operator's name for the machine, not the
    // agent's.
    void upsert(const TunnelManager::DeviceInfo& info, const QString& remark);

    std::optional<std::string> selected_device_id() const;
    std::vector<std::string> selected_device_ids() const;

signals:
    void remote_control_requested(QString device_id);
    void disconnect_requested(QStringList device_ids);
    void remark_requested(QString device_id);
    void selection_changed(QString device_id);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void activate_current();
    void show_context_menu(const QPoint& view_pos);
    void refresh_footer();
    QString detail_text(const QString& device_id) const;

    QLineEdit* search_ = nullptr;
    QListView* list_ = nullptr;
    QLabel* footer_ = nullptr;
    DeviceListModel* model_ = nullptr;
    QSortFilterProxyModel* filter_ = nullptr;
};
