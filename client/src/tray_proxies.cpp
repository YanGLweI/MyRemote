// Host side of M19: one user-IL tray proxy per active logged-in session.
//
// The host is the single source of truth (tunnel, pause flag, config). It
// spawns a copy of this same image with --tray-proxy into every session that
// has a person in it - console and RDP alike - using that session's user
// token, so the icon is drawn by the logged-in user rather than by SYSTEM.
// Every menu action comes back over the tray pipe and is applied here.

#include "tray_proxies.hpp"

#include <windows.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "desktop.hpp"
#include "log.hpp"
#include "tray_proxy.hpp"

namespace trayproxies {
namespace {

struct Client {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD session = 0;
    std::thread reader;
    std::atomic<bool> alive{true};
};

struct ProxySlot {
    HANDLE process = nullptr;
    DWORD inactive_since = 0;  // 0 = session was active last look
};

std::mutex g_mu;
std::vector<Client*> g_clients;
std::map<DWORD, ProxySlot> g_proxies;
std::thread g_acceptor;
std::thread g_supervisor;
HANDLE g_stop_event = nullptr;
CommandSink g_sink;
std::wstring g_exe;
std::string g_child_config;

std::string g_last_state_line;
bool g_tray_enabled = true;
std::string g_state_server;
std::string g_state_name;
std::atomic<bool> g_state_paused{false};
std::atomic<bool> g_state_registered{false};

std::wstring utf16(const std::string& s) { return win32util::utf8_to_wide(s); }

std::string state_line() {
    std::lock_guard<std::mutex> lock(g_mu);
    return "state paused=" + std::string(g_state_paused.load() ? "1" : "0") +
           " registered=" + std::string(g_state_registered.load() ? "1" : "0") +
           " tray_icon=" + std::string(g_tray_enabled ? "1" : "0") +
           " server=" + g_state_server + " name=" + g_state_name;
}

void write_line(HANDLE pipe, const std::string& line) {
    const std::string out = line + "\n";
    DWORD written = 0;
    WriteFile(pipe, out.data(), static_cast<DWORD>(out.size()), &written,
              nullptr);
}

void send_state_to_all(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (Client* c : g_clients) {
        if (c->alive.load()) {
            write_line(c->pipe, line);
        }
    }
}

HANDLE make_pipe_instance() {
    // SYSTEM and admins full control; any logged-in user may connect. A proxy
    // runs as its session's user, and letting that user drive pause/quit is
    // the accepted "person at the machine controls the client" boundary.
    PSECURITY_DESCRIPTOR sd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1, &sd,
        nullptr);
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    HANDLE h = CreateNamedPipeW(trayproxy::kPipeName, PIPE_ACCESS_DUPLEX,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, &sa);
    if (sd) {
        LocalFree(sd);
    }
    return h;
}

void dispatch(Client* self, const std::string& line) {
    if (line == "pause") {
        if (g_sink.pause) g_sink.pause();
    } else if (line == "resume") {
        if (g_sink.resume) g_sink.resume();
    } else if (line == "hide") {
        if (g_sink.hide) g_sink.hide();
    } else if (line == "quit") {
        if (g_sink.quit) g_sink.quit();
    } else if (line.rfind("save_config ", 0) == 0) {
        if (g_sink.save_config) {
            g_sink.save_config(line.substr(strlen("save_config ")));
        }
    } else {
        mlog::warn("Tray proxies: unknown command from session " +
                   std::to_string(self->session) + ": " + line);
    }
}

void client_reader(Client* self) {
    std::string buf;
    char chunk[512];
    DWORD n = 0;
    while (ReadFile(self->pipe, chunk, sizeof(chunk), &n, nullptr) && n) {
        buf.append(chunk, n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            const std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty()) {
                dispatch(self, line);
            }
        }
    }
    self->alive.store(false);
}

void acceptor_loop() {
    while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
        HANDLE h = make_pipe_instance();
        if (h == INVALID_HANDLE_VALUE) {
            Sleep(500);
            continue;
        }
        if (!ConnectNamedPipe(h, nullptr) &&
            GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(h);
            continue;
        }
        if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) {
            CloseHandle(h);
            break;
        }
        Client* c = new Client();
        c->pipe = h;
        DWORD pid = 0;
        GetNamedPipeClientProcessId(h, &pid);
        ProcessIdToSessionId(pid, &c->session);
        c->reader = std::thread(client_reader, c);
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_clients.push_back(c);
        }
        write_line(h, state_line());  // hello: bring the new proxy up to date
    }
}

bool launch_proxy(DWORD session) {
    HANDLE token = nullptr;
    if (!WTSQueryUserToken(session, &token)) {
        return false;  // no logged-in user there (e.g. pre-logon console)
    }
    LPVOID env = nullptr;
    CreateEnvironmentBlock(&env, token, FALSE);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"Winsta0\\Default");
    std::wstring cmd = g_exe + L" --tray-proxy --no-elevate";
    if (!g_child_config.empty()) {
        cmd += L" --config \"" + utf16(g_child_config) + L"\"";
    }
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessAsUserW(
        token, nullptr, cmd.data(), nullptr, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env, nullptr, &si, &pi);
    if (env) {
        DestroyEnvironmentBlock(env);
    }
    CloseHandle(token);
    if (!ok) {
        return false;
    }
    CloseHandle(pi.hThread);
    ProxySlot slot;
    slot.process = pi.hProcess;
    g_proxies[session] = slot;
    mlog::info("Tray proxies: spawned a proxy in session " +
               std::to_string(session));
    return true;
}

void supervisor_loop() {
    while (WaitForSingleObject(g_stop_event, 1000) != WAIT_OBJECT_0) {
        const DWORD now = GetTickCount();

        // Reap clients whose pipe closed (proxy exited).
        {
            std::lock_guard<std::mutex> lock(g_mu);
            for (size_t i = g_clients.size(); i-- > 0;) {
                Client* c = g_clients[i];
                if (!c->alive.load()) {
                    if (c->reader.joinable()) {
                        c->reader.join();
                    }
                    CloseHandle(c->pipe);
                    g_clients.erase(g_clients.begin() + i);
                    delete c;
                }
            }
        }

        std::lock_guard<std::mutex> lock(g_mu);
        const auto sessions = win32util::list_sessions();
        auto is_active = [&](DWORD id) {
            for (const auto& s : sessions) {
                if (s.id == id) {
                    return s.active && !s.user.empty();
                }
            }
            return false;
        };

        // One proxy per active session that has a user, while the icon is on.
        if (g_tray_enabled) {
            for (const auto& s : sessions) {
                if (s.id == 0 || !s.active || s.user.empty()) {
                    continue;
                }
                const auto it = g_proxies.find(s.id);
                const bool alive = it != g_proxies.end() &&
                                   WaitForSingleObject(it->second.process, 0) ==
                                       WAIT_TIMEOUT;
                if (!alive) {
                    if (it != g_proxies.end()) {
                        CloseHandle(it->second.process);
                        g_proxies.erase(it);
                    }
                    launch_proxy(s.id);
                }
            }
        }

        // Retire proxies whose session went away or whose icon was hidden.
        for (auto it = g_proxies.begin(); it != g_proxies.end();) {
            const DWORD id = it->first;
            const bool gone = !g_tray_enabled || !is_active(id);
            if (gone) {
                if (it->second.inactive_since == 0) {
                    it->second.inactive_since = now;
                } else if (static_cast<int32_t>(now - it->second.inactive_since) >
                           2000) {
                    TerminateProcess(it->second.process, 0);
                    CloseHandle(it->second.process);
                    mlog::info("Tray proxies: retired the proxy in session " +
                               std::to_string(id));
                    it = g_proxies.erase(it);
                    continue;
                }
            } else {
                it->second.inactive_since = 0;
            }
            ++it;
        }
    }
}

}  // namespace

void start(CommandSink sink, const std::string& child_config) {
    g_sink = std::move(sink);
    g_child_config = child_config;
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_exe = self;
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_acceptor = std::thread(acceptor_loop);
    g_supervisor = std::thread(supervisor_loop);
    mlog::info("Tray proxies: manager started");
}

void stop() {
    if (!g_stop_event) {
        return;
    }
    SetEvent(g_stop_event);
    // Release a blocked ConnectNamedPipe so the acceptor can observe the stop.
    HANDLE wake = CreateFileW(trayproxy::kPipeName, GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE) {
        CloseHandle(wake);
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (Client* c : g_clients) {
            if (c->alive.load()) {
                write_line(c->pipe, "bye");
            }
        }
    }
    if (g_acceptor.joinable()) {
        g_acceptor.join();
    }
    if (g_supervisor.joinable()) {
        g_supervisor.join();
    }
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& kv : g_proxies) {
        TerminateProcess(kv.second.process, 0);
        CloseHandle(kv.second.process);
    }
    g_proxies.clear();
    for (Client* c : g_clients) {
        if (c->reader.joinable()) {
            c->reader.join();
        }
        CloseHandle(c->pipe);
        delete c;
    }
    g_clients.clear();
    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
}

void broadcast_state(bool paused, bool registered, bool tray_icon,
                     const std::string& server, const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_state_paused.store(paused);
        g_state_registered.store(registered);
        g_tray_enabled = tray_icon;
        g_state_server = server;
        g_state_name = name;
    }
    const std::string line = state_line();
    static std::string last_sent;
    if (line != last_sent) {
        last_sent = line;
        send_state_to_all(line);
    }
}

std::string proxy_sessions() {
    std::lock_guard<std::mutex> lock(g_mu);
    std::string out;
    for (const auto& kv : g_proxies) {
        if (!out.empty()) {
            out += ",";
        }
        out += std::to_string(kv.first);
    }
    return out;
}

}  // namespace trayproxies
