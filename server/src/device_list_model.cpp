#include "device_list_model.hpp"

#include <QDateTime>

namespace {

QString clock(qint64 epoch_seconds) {
    if (epoch_seconds <= 0) {
        return QStringLiteral("--:--:--");
    }
    return QDateTime::fromSecsSinceEpoch(epoch_seconds).toString("HH:mm:ss");
}

}  // namespace

DeviceListModel::DeviceListModel(QObject* parent) : QAbstractTableModel(parent) {}

void DeviceListModel::upsert(const Entry& entry) {
    const auto existing = row_of_.constFind(entry.sid);
    if (existing != row_of_.constEnd()) {
        entries_[existing.value()] = entry;
        const int row = existing.value();
        emit dataChanged(index(row, 0), index(row, Column - 1));
        return;
    }
    beginInsertRows(QModelIndex(), static_cast<int>(entries_.size()),
                    static_cast<int>(entries_.size()));
    row_of_.insert(entry.sid, static_cast<int>(entries_.size()));
    entries_.push_back(entry);
    endInsertRows();
}

int DeviceListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

int DeviceListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : Column;
}

QVariant DeviceListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= static_cast<int>(entries_.size())) {
        return QVariant();
    }
    const Entry& entry = entries_[index.row()];
    switch (role) {
        case Qt::DisplayRole:
        case NameRole:
            return entry.name;
        case SidRole:
            return entry.sid;
        case MetaRole:
            return meta_text(entry);
        case StateRole:
            return static_cast<int>(entry.state);
        case BadgesRole:
            return badges(entry);
        case Qt::TextAlignmentRole:
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        default:
            return QVariant();
    }
}

QString DeviceListModel::meta_text(const Entry& entry) const {
    QString text = QStringLiteral("%1 · %2x%3 · %4")
                       .arg(entry.ip.isEmpty() ? QStringLiteral("未知地址") : entry.ip)
                       .arg(entry.width)
                       .arg(entry.height)
                       .arg(state_word(entry.state));
    if (entry.state == DeviceState::Live) {
        text += QStringLiteral(" %1").arg(clock(entry.connect_time));
    } else {
        text += QStringLiteral(" · 上次 %1").arg(clock(entry.last_seen_time));
    }
    return text;
}

std::optional<DeviceListModel::Entry> DeviceListModel::entry(
    const QString& sid) const {
    const auto row = row_of_.constFind(sid);
    if (row == row_of_.constEnd()) {
        return std::nullopt;
    }
    return entries_[row.value()];
}

int DeviceListModel::live_count() const {
    int live = 0;
    for (const Entry& entry : entries_) {
        if (entry.state == DeviceState::Live) {
            ++live;
        }
    }
    return live;
}

QString DeviceListModel::state_word(DeviceState state) {
    switch (state) {
        case DeviceState::Live:
            return QStringLiteral("已连接");
        case DeviceState::Reconnecting:
            return QStringLiteral("重连中");
        case DeviceState::Offline:
            return QStringLiteral("离线");
    }
    return QStringLiteral("未知");
}

QStringList DeviceListModel::badges(const Entry& entry) {
    // Only the things that change what the operator can do with this machine.
    QStringList badges;
    if (entry.flags & proto::kFlagServiceHost) {
        badges << QStringLiteral("服务");
    }
    if (entry.flags & proto::kFlagLogonScreen) {
        badges << QStringLiteral("登录界面");
    } else if (entry.elevation_known &&
               !(entry.flags & proto::kFlagConsoleOwner)) {
        badges << QStringLiteral("非控制台");
    }
    if (entry.elevation_known && !entry.elevated) {
        // UIPI drops its SendInput on elevated windows, which otherwise looks
        // exactly like a dead session.
        badges << QStringLiteral("受限");
    }
    return badges;
}

QString DeviceListModel::badge_text(const TunnelManager::DeviceInfo& info) {
    Entry entry;
    entry.flags = info.flags;
    entry.elevated = info.elevated;
    entry.elevation_known = info.elevation_known;
    QStringList badges = DeviceListModel::badges(entry);
    QString text;
    for (const QString& badge : badges) {
        text += QStringLiteral("〔%1〕").arg(badge);
    }
    return text;
}

QString DeviceListModel::display_name(const TunnelManager::DeviceInfo& info,
                                     const QString& remark) {
    // The remark wins: it is the name the operator chose.
    const QString name =
        remark.isEmpty() ? QString::fromStdString(info.device_name) : remark;
    return name + QStringLiteral(" ") + badge_text(info);
}
