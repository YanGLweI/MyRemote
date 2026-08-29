#include "log_tail.hpp"

#include <QCoreApplication>
#include <QMetaObject>

#include "log.hpp"

LogTail::LogTail(QObject* parent) : QObject(parent) {
    mlog::set_sink([this](mlog::Level level, const std::string& line) {
        const QString text = QString::fromStdString(line);
        const int kind = static_cast<int>(level);
        // Queued rather than direct: this call can come from any thread, and a
        // drawer that repaints mid-write would put the GUI inside the logging
        // lock for the length of a paint.
        QMetaObject::invokeMethod(
            this, [this, text, kind] { emit line_logged(text, kind); },
            Qt::QueuedConnection);
    });
}

LogTail::~LogTail() {
    mlog::set_sink(nullptr);
}
