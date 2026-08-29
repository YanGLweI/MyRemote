// MyRemote Control Center (控制端)
// Passive side only: lists clients that connected out to us, and drives
// remote sessions over those client-initiated tunnels.

#include <windows.h>

#include <QApplication>

#include <string>

#include "config.hpp"
#include "log.hpp"
#include "log_tail.hpp"
#include "main_window.hpp"
#include "theme.hpp"

namespace {

std::string exe_dir() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    size_t pos = s.find_last_of("\\/");
    return pos == std::string::npos ? "." : s.substr(0, pos);
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    QApplication app(__argc, __argv);
    QApplication::setApplicationName(QStringLiteral("MyRemote Control Center"));
    // Before any widget exists: the style and font are fixed at first polish.
    theme::apply(app);

    std::string dir = exe_dir();
    mlog::init(dir + "/" + "control_server.log");
    // Before the first line, and declared before the window so it is destroyed
    // after it: the tunnel threads are still logging while the window closes.
    LogTail log_tail;
    mlog::info("Control center starting");

    config::ServerConfig cfg =
        config::ServerConfig::load(dir + "/server_config.json");

    MainWindow window(cfg, log_tail);
    window.show();
    return app.exec();
}
