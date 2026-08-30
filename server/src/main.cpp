// MyRemote Control Center (控制端)
// Passive side only: lists clients that connected out to us, and drives
// remote sessions over those client-initiated tunnels.

#include <windows.h>

#include <QApplication>

#include <cstdio>
#include <string>

#include "app_paths.hpp"
#include "config.hpp"
#include "i18n.hpp"
#include "log.hpp"
#include "log_tail.hpp"
#include "main_window.hpp"
#include "theme.hpp"

namespace {

// A GUI-subsystem exe usually arrives with no standard handles at all, so borrow
// the console it was started from. If the caller already handed us one - a
// redirected file or pipe - writing there is the whole point, and reopening
// CONOUT$ would throw that redirect away.
void attach_parent_console() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) {
        return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        return;
    }
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Answered before QApplication, the config and the log exist: a version
    // query must not bind the port or leave a line in the record.
    for (int i = 1; i < __argc; ++i) {
        if (std::string("--version") == __argv[i]) {
            attach_parent_console();
            printf("MyRemote control server %s\n", MYREMOTE_VERSION);
            return 0;
        }
    }
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
