// MyRemote Control Center (控制端)
// Passive side only: lists clients that connected out to us, and drives
// remote sessions over those client-initiated tunnels.

#include <windows.h>

#include <QApplication>

#include <string>

#include "config.hpp"
#include "log.hpp"
#include "main_window.hpp"

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

    std::string dir = exe_dir();
    mlog::init(dir + "/" + "control_server.log");
    mlog::info("Control center starting");

    config::ServerConfig cfg =
        config::ServerConfig::load(dir + "/server_config.json");

    MainWindow window(cfg);
    window.show();
    return app.exec();
}
