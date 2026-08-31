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
// Set when the host's state says the icon should not exist any more, so the
// read loop can take itself down instead of being killed from outside.
std::atomic<bool> g_leave{false};
// 0 = nothing asked yet, 1 = the host wrote it, -1 = the host refused. Only the
// host can write this file, so "save_config" is a request, not an achievement.
std::atomic<int> g_save_reply{0};
constexpr DWORD kSilenceMs = 15000;

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
    // "tray_icon=0" is the host saying this icon should not exist. Doing it
    // here is what lets the host stop killing proxies, which is how ghost
    // icons got painted in the first place.
    if (field("tray_icon=") == "0") {
        g_leave.store(true);
    }
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
    // Before the pipe: a proxy that never reached its host still has to say
    // which config it was handed. This process used to say nothing at all, so
    // "the dialog showed 127.0.0.1" left no trace to diagnose from.
    const win32util::AgentPaths paths =
        win32util::resolve_paths(config_override);
    g_config_path = paths.config;
    mlog::info("Tray proxy config: " + g_config_path +
               (paths.config_present ? " (found)" : " (missing)"));

    HANDLE pipe = connect_to_host();
    if (pipe == INVALID_HANDLE_VALUE) {
        mlog::warn("Tray proxy: no host pipe; giving up and exiting");
        return 1;
    }
    g_pipe = pipe;
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
    mlog::info("Tray proxy: connected to the session host");

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
        config::LoadStatus status = config::LoadStatus::Missing;
        const config::ClientConfig c =
            config::ClientConfig::load(g_config_path, &status);
        if (status == config::LoadStatus::Unreadable) {
            // A dialog full of defaults here is a lie with teeth: the host
            // answers Save by writing this very file. Missing is different - that
            // is what a machine nobody has configured yet looks like.
            g_dialog_open.store(false);
            mlog::warn("Tray proxy: not editing an unreadable config: " +
                       g_config_path);
            const std::wstring text =
                L"读不到配置文件里的任何设置：\n" +
                win32util::utf8_to_wide(g_config_path) +
                L"\n\n在这个窗口里保存会把默认值覆盖上去，所以它没有打开。"
                L"请先修正这个文件，或者删掉它再重新配置。";
            MessageBoxW(nullptr, text.c_str(), L"MyRemote 配置",
                        MB_OK | MB_ICONERROR);
            return;
        }
        gui::ConfigUi ui;
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
        // writes on our behalf and hot-reloads itself. Its answer is the only
        // evidence the write happened, so wait for it.
        ui.save_via = [](const config::ClientConfig& c2) {
            g_save_reply.store(0);
            if (!send_line("save_config " + to_json_line(c2))) {
                return false;
            }
            for (int i = 0; i < 60 && g_save_reply.load() == 0; ++i) {
                Sleep(50);
            }
            if (g_save_reply.load() == 0) {
                mlog::warn("Tray proxy: the agent never answered the save");
            }
            return g_save_reply.load() > 0;
        };
        gui::show_config_gui_async(std::move(ui),
                                   [](const gui::ConfigUi&) {
                                       g_dialog_open.store(false);
                                   });
    };
    if (!tray.start(std::move(actions))) {
        mlog::warn("Tray proxy: icon unavailable in this session");
    }

    // Poll rather than block in ReadFile: this loop also has to notice a host
    // that stopped talking. A replaced or wedged host keeps its pipe instance
    // open, so a blocking read would sit behind a painted icon forever - the
    // exact state this milestone exists to end.
    std::string buf;
    DWORD last_rx = GetTickCount();
    const char* reason = "host pipe closed";
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
            break;
        }
        if (avail) {
            char chunk[1024];
            DWORD n = 0;
            if (!ReadFile(pipe, chunk, sizeof(chunk) - 1, &n, nullptr) || !n) {
                break;
            }
            buf.append(chunk, n);
            last_rx = GetTickCount();
            bool leave = false;
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                const std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                if (line == "bye" || line == "die") {
                    reason = line == "bye" ? "host said bye" : "host retired this icon";
                    leave = true;
                    break;
                }
                if (line == "ping") {
                    // liveness() only advances inside the pump's message loop,
                    // so this is the host's proof that the tray can still answer.
                    send_line("pong tray=" + std::to_string(tray.liveness()));
                    continue;
                }
                if (line.rfind("saved ", 0) == 0) {
                    g_save_reply.store(line == "saved ok" ? 1 : -1);
                    continue;
                }
                if (line.rfind("state ", 0) == 0) {
                    apply_state(tray, line);
                }
            }
            if (leave) {
                break;
            }
        }
        if (g_leave.load()) {
            reason = "the host hid the icon";
            break;
        }
        if (static_cast<int32_t>(GetTickCount() - last_rx) > kSilenceMs) {
            reason = "the host stopped talking";
            mlog::warn("Tray proxy: nothing from the host for 15s; exiting so a live host can respawn us");
            break;
        }
        Sleep(150);
    }
    mlog::info(std::string("Tray proxy: ") + reason + "; exiting");
    CloseHandle(pipe);
    g_pipe = INVALID_HANDLE_VALUE;
    tray.stop();
    return 0;
}

}  // namespace trayproxy
