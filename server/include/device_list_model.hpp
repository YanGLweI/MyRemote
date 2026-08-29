#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QStringList>

#include <ctime>
#include <optional>
#include <vector>

#include "tunnel_manager.hpp"

// The roster as data only: one row per device seen this run, formatted text
// included, so the delegate has nothing to decide but layout.
class DeviceListModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Role {
        SidRole = Qt::UserRole + 1,  // device_id, the row's key
        NameRole,                    // remark when there is one, else hostname
        MetaRole,                    // ip · 分辨率 · 状态与时刻
        StateRole,                   // int DeviceState
        BadgesRole,                  // QStringList of badge words, no brackets
    };
    enum { Column = 1 };  // one column: the delegate paints the whole row

    struct Entry {
        QString sid;
        QString name;
        QString host;
        QString remark;
        QString ip;
        int width = 0;
        int height = 0;
        DeviceState state = DeviceState::Offline;
        time_t connect_time = 0;
        time_t last_seen_time = 0;
        uint8_t flags = 0;
        bool elevated = false;
        bool elevation_known = false;
    };

    explicit DeviceListModel(QObject* parent = nullptr);

    // Rows never move: an operator's muscle memory points at a machine, and a
    // row that jumps or blinks when its tunnel drops reads as a broken console.
    void upsert(const Entry& entry);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    std::optional<Entry> entry(const QString& sid) const;
    int live_count() const;

    // Badge vocabulary, shared with the tab titles so a machine cannot be
    // described two ways in one window.
    static QString state_word(DeviceState state);
    static QStringList badges(const Entry& entry);
    static QString badge_text(const TunnelManager::DeviceInfo& info);
    static QString display_name(const TunnelManager::DeviceInfo& info,
                                const QString& remark);

private:
    QString meta_text(const Entry& entry) const;

    std::vector<Entry> entries_;
    QHash<QString, int> row_of_;
};
