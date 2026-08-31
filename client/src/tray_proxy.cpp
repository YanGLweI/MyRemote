// M19 tray proxy: one user-IL tray process per logged-in session. It draws
// the notification-area icon in the session it was spawned in (console or
// RDP) and forwards every menu action to the session host over the tray
// pipe; privileged effects (pause the tunnel, stop the service, write the
// config) all happen host-side. When the pipe closes, the host is gone and
// this process has no reason to exist.

#include "tray_proxy.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>

#include "config.hpp"
#include "config_gui.hpp"
#include "desktop.hpp"
#include "log.hpp"
#include "tray_icon.hpp"

namespace trayproxy {
namespace {

struct HostState {
    std::atomic<bool> paused{false};
    std::atomic<bool> registered{false};
    std::mutex mu;
    std::wstring name;
    std::wstring server;
};

HANDLE g_pipe = INVALID_HANDLE_VALUE;
std::mutex g_write_mu;
std::atomic<bool> g_dialog_open{false};

bool send_line(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_write_mu);
    if (g_pipe == INVALID_HANDLE_VALUE) {
        return false;
    }
    const std::string out = line + "\n";
    DWORD written = 0;
    return WriteFile(g_pipe, out.data(), static_cast<DWORD>(out.size()),
                     &written, nullptr) != FALSE;
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

// One line, no newlines: the pipe framing is line-based, and the host parses
// it with the same flat JSON reader that reads config.json.
std::string to_json_line(const config::ClientConfig& c) {
    return "{\"server_ip\": \"" + json_escape(c.server_ip) +
           "\", \"server_port\": " + std::to_string(c.server_port) +
           ", \"secret_key\": \"" + json_escape(c.secret_key) +
           "\", \"device_name\": \"" + json_escape(c.device_name) +
           "\", \"control_password\": \"" + json_escape(c.control_password) +
           "\", \"max_encode_width\": " + std::to_string(c.max_encode_width) +
           ", \"tray_icon\": " + (c.tray_icon ? "true" : "false") + "}";
}

// The menu actions run on TrayIcon's worker thread, which can outlive run()'s
// stack frame, so everything they read lives here instead.
HostState g_state;
std::string g_config_path;

// "state paused=0 registered=1 tray_icon=1 server=ip:port name=..." - name is
// last because it is the only value allowed to contain spaces.
void apply_state(TrayIcon& tray, const std::string& line) {
    auto field = [&](const char* key) -> std::string {
        const size_t at = line.find(key);
        if (at == std::string::npos) {
            return {};
        }
        size_t begin = at + std::string(key).size();
        const size_t end = line.find(' ', begin);
        return line.substr(begin, end == std::string::npos ? end : end - begin);
    };
    g_state.paused.store(field("paused=") == "1");
    g_state.registered.store(field("registered=") == "1");
    const std::string server = field("server=");
    const size_t np = line.find("name=");
    const std::string name =
        np == std::string::npos ? "" : line.substr(np + 5);
    {
        std::lock_guard<std::mutex> lock(g_state.mu);
        g_state.server = win32util::utf8_to_wide(server);
        g_state.name = win32util::utf8_to_wide(name);
    }
    std::wstring tip;
    {
        std::lock_guard<std::mutex> lock(g_state.mu);
        tip = g_state.name + L" | " + g_state.server;
    }
    tip += g_state.paused.load() ? L" | 已停止，本机不被远控"
           : (g_state.registered.load() ? L" | 已注册" : L" | 连接/重试中");
    tray.set_tooltip(tip);
}

HANDLE connect_to_host() {
    // The proxy is only ever spawned by a host that already created the pipe,
    // so this covers a startup race, not a long wait: a dead host must not
    // leave a zombie proxy hanging around for minutes.
    int backoff_ms = 100;
    for (int attempt = 0; attempt < 15; ++attempt) {
        HANDLE h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            return h;
        }
        WaitNamedPipeW(kPipeName, backoff_ms);
        backoff_ms = std::min(backoff_ms * 2, 1000);
    }
    return INVALID_HANDLE_VALUE;
}

void init_proxy_log() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const std::string file = "tray-" + std::to_string(session) + ".log";
    const std::wstring dir = win32util::program_data_dir();
    if (!dir.empty() &&
        mlog::init(win32util::wide_to_utf8(dir.c_str()) + "\\" + file)) {
        return;
    }
    wchar_t temp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temp)) {
        mlog::init(win32util::wide_to_utf8(temp) + "MyRemote-" + file);
    }
}

}  // namespace

int run(const std::string& config_override) {
    init_proxy_log();
    HANDLE pipe = connect_to_host();
    if (pipe == INVALID_HANDLE_VALUE) {
        mlog::warn("Tray proxy: no host pipe; giving up and exiting");
        return 1;
    }
    g_pipe = pipe;
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
    mlog::info("Tray proxy: connected to the session host");

    g_config_path = win32util::resolve_paths(config_override).config;

    TrayIcon tray;
    TrayIcon::Actions actions;
    actions.paused = [] { return g_state.paused.load(); };
    actions.toggle_pause = [] {
        // The label was chosen on the tray thread; decide the word from the
        // same flag and flip it now, or a second click while the host's echo is
        // still in flight would send "pause" twice.
        const bool now_paused = !g_state.paused.load();
        g_state.paused.store(now_paused);
        send_line(now_paused ? "pause" : "resume");
    };
    actions.hide_icon = [] { send_line("hide"); };
    actions.quit = [] { send_line("quit"); };
    actions.quit_text = L"退出（同时停止服务，下次开机自动回来）";
    actions.open_config = [] {
        bool expected = false;
        if (!g_dialog_open.compare_exchange_strong(expected, true)) {
            return;
        }
        gui::ConfigUi ui;
        const config::ClientConfig c =
            config::ClientConfig::load(g_config_path);
        ui.server_ip = c.server_ip;
        ui.server_port = c.server_port;
        ui.secret_key = c.secret_key;
        ui.device_name = c.device_name;
        ui.control_password = c.control_password;
        ui.tray_icon = c.tray_icon;
        ui.max_encode_width = c.max_encode_width;
        ui.config_path = g_config_path;
        ui.save_mode = gui::SaveMode::SaveAndApply;
        // The proxy may not be able to write the agent directory; the host
        // writes on our behalf and hot-reloads itself.
        ui.save_via = [](const config::ClientConfig& c2) {
            return send_line("save_config " + to_json_line(c2));
        };
        gui::show_config_gui_async(std::move(ui),
                                   [](const gui::ConfigUi&) {
                                       g_dialog_open.store(false);
                                   });
    };
    if (!tray.start(std::move(actions))) {
        mlog::warn("Tray proxy: icon unavailable in this session");
    }

    // This thread becomes the pipe reader; the tray pumps on its own thread.
    std::string buf;
    char chunk[512];
    DWORD n = 0;
    bool bye = false;
    while (!bye && ReadFile(pipe, chunk, sizeof(chunk), &n, nullptr) && n) {
        buf.append(chunk, n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            const std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (line == "bye") {
                bye = true;
                break;
            }
            if (line.rfind("state ", 0) == 0) {
                apply_state(tray, line);
            }
        }
    }
    mlog::info(bye ? "Tray proxy: host said bye; exiting"
                   : "Tray proxy: host pipe closed; exiting");
    CloseHandle(pipe);
    g_pipe = INVALID_HANDLE_VALUE;
    tray.stop();
    return 0;
}

}  // namespace trayproxy
