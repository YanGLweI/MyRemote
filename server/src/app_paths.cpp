#include "app_paths.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QString>

#include <string>

namespace app {

namespace {

QDir exe_dir_q() { return QDir(QCoreApplication::applicationDirPath()); }

}  // namespace

std::string exe_dir() { return exe_dir_q().absolutePath().toStdString(); }

std::string config_path() {
    return exe_dir_q().filePath(QStringLiteral("server_config.json")).toStdString();
}

std::string resolve_log_path(const std::string& configured) {
    const QString name = QString::fromStdString(configured).trimmed();
    if (name.isEmpty()) {
        return {};
    }
    if (QDir::isAbsolutePath(name)) {
        return name.toStdString();
    }
    return exe_dir_q().filePath(name).toStdString();
}

}  // namespace app
