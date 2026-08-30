// MyRemote Control Center (控制端)
// Passive side only: lists clients that connected out to us, and drives
// remote sessions over those client-initiated tunnels.

#include <windows.h>

#include <QApplication>

#include <cstdio>
#include <cwchar>
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

// A zh-CN console is not UTF-8, so printf-ing Chinese lands as mojibake. Write
// the wide string straight to the console when stdout is one, and only encode
// to UTF-8 when it is a redirected file or pipe - where UTF-8 is the point.
void print_line(const wchar_t* text) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) {
        return;
    }
    const size_t chars = wcslen(text);
    DWORD written = 0;
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode)) {
        WriteConsoleW(out, text, static_cast<DWORD>(chars), &written, nullptr);
        return;
    }
    std::string utf8;
    utf8.resize(static_cast<size_t>(WideCharToMultiByte(
        CP_UTF8, 0, text, static_cast<int>(chars), nullptr, 0, nullptr, nullptr)));
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(chars),
                        utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
    WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

// Held for the whole process on purpose: the name existing *is* the lock, so
// closing the handle would release it while we are still listening.
HANDLE g_instance_lock = nullptr;

BOOL CALLBACK raise_running_window(HWND hwnd, LPARAM lparam) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() || pid == 0) {
        return TRUE;
    }
    if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;  // a dialog belongs to that other window; raising it is not the point
    }
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    wchar_t title[128] = {};
    if (GetWindowTextW(hwnd, title, 128) <= 0 ||
        wcscmp(title, app::kWindowTitle) != 0) {
        return TRUE;
    }
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    if (!SetForegroundWindow(hwnd)) {
        // The foreground lock is allowed to refuse us; this is the documented
        // way round it, and it beats relaxing the lock for every process.
        SwitchToThisWindow(hwnd, TRUE);
    }
    *reinterpret_cast<bool*>(lparam) = true;
    return FALSE;
}

// False means somebody else already holds this machine's control centre.
bool acquire_instance_lock() {
    // Global\, not session-local: what is being protected is the TCP listen
    // port, and that is machine-wide. A per-session name would let the operator
    // at the console and one inside RDP each bind 7500 - the exact bug.
    g_instance_lock = CreateMutexW(
        nullptr, TRUE, L"Global\\MyRemoteControlCenter_SingleInstance");
    if (!g_instance_lock) {
        // Creating under Global\ can be refused; failing to guard is better
        // than refusing to start, so fall back to the bare name.
        g_instance_lock = CreateMutexW(
            nullptr, TRUE, L"MyRemoteControlCenter_SingleInstance");
        if (!g_instance_lock) {
            return true;
        }
    }
    return GetLastError() != ERROR_ALREADY_EXISTS;
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
    // Before QApplication: a second launch must not paint a window that then
    // discovers the port is taken, and --force is the developer's way to run two
    // builds against two ports (server_config.json sits next to each exe).
    bool forced = false;
    for (int i = 1; i < __argc; ++i) {
        if (std::string("--force") == __argv[i]) {
            forced = true;
        }
    }
    if (!forced && !acquire_instance_lock()) {
        attach_parent_console();
        bool raised = false;
        EnumWindows(raise_running_window, reinterpret_cast<LPARAM>(&raised));
        print_line(raised ? L"已有控制中心在运行，已把它切到前台。\n"
                          : L"已有控制中心在运行，这个端口归它。\n");
        return 0;
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
