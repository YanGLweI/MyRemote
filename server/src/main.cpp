// MyRemote Control Center (控制端)
// Passive side only: lists clients that connected out to us, and drives
// remote sessions over those client-initiated tunnels.

#include <windows.h>

#include <QApplication>

#include <string>

#include "app_paths.hpp"
#include "config.hpp"
#include "i18n.hpp"
#include "log.hpp"
#include "log_tail.hpp"
#include "main_window.hpp"
#include "theme.hpp"

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    QApplication app(__argc, __argv);
    QApplication::setApplicationName(QStringLiteral("MyRemote Control Center"));
    // Before the first dialog is built: translators are consulted per string,
    // at the moment the widget asks.
    i18n::install_button_translator(app);
    // Before any widget exists: the style and font are fixed at first polish.
    theme::apply(app);

    // The config decides where the log goes, so it has to be read first.
    config::ServerConfig cfg = config::ServerConfig::load(app::config_path());
    const std::string wanted = app::resolve_log_path(cfg.log_file);
    const bool writing = mlog::init(wanted);
    // Before the first line, and declared before the window so it is destroyed
    // after it: the tunnel threads are still logging while the window closes.
    LogTail log_tail;
    if (!wanted.empty() && !writing) {
        // No fallback to a name the operator did not ask for: say so on the
        // record instead, where the drawer and the problem count will show it.
        mlog::warn(QStringLiteral("日志文件打不开（%1），这次只记在窗口里")
                       .arg(QString::fromStdString(wanted))
                       .toStdString());
    }
    mlog::info("Control center starting");

    MainWindow window(cfg, log_tail);
    window.show();
    return app.exec();
}
