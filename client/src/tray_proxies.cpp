// Host side of M19: one user-IL tray proxy per active logged-in session.
//
// The host is the single source of truth (tunnel, pause flag, config). It
// spawns a copy of this same image with --tray-proxy into every session that
// has a person in it - console and RDP alike - using that session's user
// token, so the icon is drawn by the logged-in user rather than by SYSTEM.
// Every menu action comes back over the tray pipe and is applied here.
//
// Two rules run through this file, and M20 exists because they were broken:
//  - nothing on the host's own threads may block waiting for a proxy, because
//    the supervisor is the thing that repairs a bad proxy;
//  - a proxy is asked to leave (bye / die / cut the pipe) and only as a last
//    resort killed, because Shell_NotifyIconW(NIM_DELETE) can only be done by
//    the process that added the icon - killing one leaves a painted icon that
//    answers nothing, which reads as a working tray.

#include "tray_proxies.hpp"

#include <windows.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "desktop.hpp"
#include "log.hpp"
#include "tray_proxy.hpp"

namespace trayproxies {
namespace {

constexpr int kQueueLimit = 32;           // lines, per proxy
constexpr DWORD kPingEveryMs = 2000;
constexpr DWORD kPongTimeoutMs = 8000;    // no pong at all -> cut the pipe
constexpr int kStalePongs = 3;            // pongs, but the pump is not moving
constexpr DWORD kExitGraceMs = 3000;
constexpr DWORD kAfterCutGraceMs = 2000;

struct Client {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD session = 0;
    std::atomic<bool> alive{true};
    std::atomic<bool> shutting_down{false};
    std::thread reader;
    std::thread writer;
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<std::string> queue;
    std::atomic<DWORD> since{0};             // when we accepted it
    std::atomic<DWORD> last_pong_tick{0};
    std::atomic<unsigned long long> last_pong_liveness{0};
    std::atomic<int> stale_pongs{0};
};

std::mutex g_mu;
std::vector<Client*> g_clients;
std::map<DWORD, HANDLE> g_proxies;   // session -> process handle
std::map<DWORD, DWORD> g_retiring;   // session -> tick we first asked it to leave
std::thread g_acceptor;
std::thread g_supervisor;
HANDLE g_stop_event = nullptr;
CommandSink g_sink;
std::wstring g_exe;
std::string g_child_config;

bool g_tray_enabled = true;
std::string g_state_server;
std::string g_state_name;
std::atomic<bool> g_state_paused{false};
std::atomic<bool> g_state_registered{false};
DWORD g_last_ping_tick = 0;

std::wstring utf16(const std::string& s) { return win32util::utf8_to_wide(s); }

std::string state_line() {
    std::lock_guard<std::mutex> lock(g_mu);
    return "state paused=" + std::string(g_state_paused.load() ? "1" : "0") +
           " registered=" + std::string(g_state_registered.load() ? "1" : "0") +
           " tray_icon=" + std::string(g_tray_enabled ? "1" : "0") +
           " server=" + g_state_server + " name=" + g_state_name;
}

// Never called with g_mu held: a proxy that stopped draining must be able to
// hold up nothing but its own queue.
void queue_line(Client* c, const std::string& line) {
    if (!c->alive.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(c->qmu);
        if (c->queue.size() >= kQueueLimit) {
            // It cannot drain faster than we talk to it. Drop the client rather
            // than grow without bound; the supervisor will cut and respawn it.
            c->alive.store(false);
            return;
        }
        c->queue.push_back(line);
    }
    c->qcv.notify_one();
}

bool raw_write(HANDLE pipe, const std::string& line) {
    const std::string out = line + "\n";
    DWORD written = 0;
    return WriteFile(pipe, out.data(), static_cast<DWORD>(out.size()), &written,
                     nullptr) != FALSE;
}

void writer_loop(Client* c) {
    for (;;) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(c->qmu);
            c->qcv.wait(lock, [&] {
                return c->shutting_down.load() || !c->queue.empty();
            });
            if (c->queue.empty()) {
                if (c->shutting_down.load()) {
                    return;
                }
                continue;
            }
            line = std::move(c->queue.front());
            c->queue.pop_front();
        }
        if (!raw_write(c->pipe, line)) {
            c->alive.store(false);
            return;
        }
    }
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
    } else if (line.rfind("pong tray=", 0) == 0) {
        // Proof that the proxy's tray message loop is still turning: the number
        // is a counter only that loop can advance.
        const unsigned long long ticks =
            _strtoui64(line.c_str() + strlen("pong tray="), nullptr, 10);
        self->last_pong_tick.store(GetTickCount());
        if (self->last_pong_liveness.exchange(ticks) == ticks) {
            self->stale_pongs.fetch_add(1);
        } else {
            self->stale_pongs.store(0);
        }
    } else if (line.rfind("save_config ", 0) == 0) {
        if (g_sink.save_config) {
            g_sink.save_config(line.substr(strlen("save_config ")));
        }
    } else {
        mlog::warn("Tray proxies: unknown command from session " +
                   std::to_string(self->session) + ": " + line);
    }
}

void reader_loop(Client* c) {
    std::string buf;
    char chunk[512];
    while (!c->shutting_down.load()) {
        DWORD avail = 0;
        if (!PeekNamedPipe(c->pipe, nullptr, 0, nullptr, &avail, nullptr)) {
            break;  // the proxy exited or the pipe broke
        }
        if (!avail) {
            Sleep(150);
            continue;
        }
        DWORD n = 0;
        if (!ReadFile(c->pipe, chunk, sizeof(chunk), &n, nullptr) || !n) {
            break;
        }
        buf.append(chunk, n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            const std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty()) {
                dispatch(c, line);
            }
        }
    }
    c->alive.store(false);
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
        const DWORD now = GetTickCount();
        c->since.store(now);
        c->last_pong_tick.store(now);
        c->reader = std::thread(reader_loop, c);
        c->writer = std::thread(writer_loop, c);
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_clients.push_back(c);
        }
        queue_line(c, state_line());  // hello: bring the new proxy up to date
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
    std::lock_guard<std::mutex> lock(g_mu);
    g_proxies[session] = pi.hProcess;
    mlog::info("Tray proxies: spawned a proxy in session " +
               std::to_string(session));
    return true;
}

void close_client(Client* c) {
    c->shutting_down.store(true);
    c->qcv.notify_all();
    if (c->reader.joinable()) {
        c->reader.join();
    }
    if (c->writer.joinable()) {
        c->writer.join();
    }
    CloseHandle(c->pipe);
    c->pipe = INVALID_HANDLE_VALUE;
    delete c;
}

// Cut the pipe instead of killing the process: the proxy's own read loop turns
// the broken pipe into a clean exit, and a clean exit is the only moment it can
// delete its icon.
void cut_pipe(Client* c, const char* why) {
    mlog::info(std::string("Tray proxies: cutting the pipe to session ") +
               std::to_string(c->session) + " (" + why + ")");
    c->shutting_down.store(true);
    c->qcv.notify_all();
    DisconnectNamedPipe(c->pipe);
}

// Ask the proxy to leave, give it time to do so, and only then escalate. The
// escalation is logged because it is the one path that can leave a ghost icon.
void retire_proxy(DWORD session, HANDLE process, const char* why) {
    if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
        mlog::info(std::string("Tray proxies: retiring the proxy in session ") +
                   std::to_string(session) + " (" + why + ")");
    }
    if (WaitForSingleObject(process, kExitGraceMs) == WAIT_TIMEOUT) {
        TerminateProcess(process, 0);
        WaitForSingleObject(process, 1000);
        mlog::error("Tray proxies: session " + std::to_string(session) +
                    " proxy had to be killed; it may have left a ghost icon");
    }
    CloseHandle(process);
}

void supervisor_loop() {
    while (WaitForSingleObject(g_stop_event, 1000) != WAIT_OBJECT_0) {
        const DWORD now = GetTickCount();

        // Ping every proxy so a proxy whose pump stopped turning is visible.
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (static_cast<int32_t>(now - g_last_ping_tick) >= (int)kPingEveryMs) {
                g_last_ping_tick = now;
                for (Client* c : g_clients) {
                    queue_line(c, "ping");
                }
            }
        }

        // Reap clients whose pipe closed, and ones that stopped answering.
        std::vector<Client*> dead;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            for (size_t i = g_clients.size(); i-- > 0;) {
                Client* c = g_clients[i];
                const bool silent =
                    static_cast<int32_t>(now - c->last_pong_tick.load()) >
                    (int)kPongTimeoutMs;
                const bool stuck_pump =
                    c->stale_pongs.load() >= kStalePongs;
                if (!c->alive.load() || silent || stuck_pump) {
                    if (c->alive.load() && !c->shutting_down.load()) {
                        cut_pipe(c, silent ? "no pong" : "tray pump not turning");
                    }
                    if (!c->alive.load()) {
                        dead.push_back(c);
                        g_clients.erase(g_clients.begin() + i);
                    }
                }
            }
        }
        for (Client* c : dead) {
            close_client(c);
        }

        std::vector<DWORD> to_spawn;
        bool tray_enabled;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            tray_enabled = g_tray_enabled;
        }
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
        // Spawning happens outside the lock: CreateProcessAsUserW is slow and
        // the broadcast path must not wait on it.
        if (tray_enabled) {
            for (const auto& s : sessions) {
                if (s.id == 0 || !s.active || s.user.empty()) {
                    continue;
                }
                bool alive = false;
                {
                    std::lock_guard<std::mutex> lock(g_mu);
                    const auto it = g_proxies.find(s.id);
                    alive = it != g_proxies.end() &&
                            WaitForSingleObject(it->second, 0) == WAIT_TIMEOUT;
                }
                if (!alive) {
                    to_spawn.push_back(s.id);
                }
            }
        }
        for (DWORD id : to_spawn) {
            launch_proxy(id);
        }

        // Retire proxies whose session went away or whose icon was hidden. The
        // first pass tells the proxy to leave and cuts its pipe, which is how it
        // gets the chance to delete its own icon; the kill is the last resort.
        std::vector<std::pair<DWORD, HANDLE>> to_retire;
        std::vector<Client*> to_cut;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            for (auto it = g_proxies.begin(); it != g_proxies.end();) {
                const DWORD id = it->first;
                const bool gone = !tray_enabled || !is_active(id);
                if (!gone) {
                    g_retiring.erase(id);
                    ++it;
                    continue;
                }
                const auto first = g_retiring.find(id);
                if (first == g_retiring.end()) {
                    g_retiring[id] = now;
                    for (Client* c : g_clients) {
                        if (c->session == id) {
                            to_cut.push_back(c);
                        }
                    }
                    ++it;
                    continue;
                }
                if (static_cast<int32_t>(now - first->second) >
                    (int)kAfterCutGraceMs) {
                    to_retire.push_back({id, it->second});
                    g_retiring.erase(id);
                    it = g_proxies.erase(it);
                    continue;
                }
                ++it;
            }
        }
        for (Client* c : to_cut) {
            cut_pipe(c, "session left or icon hidden");
        }
        for (const auto& p : to_retire) {
            retire_proxy(p.first, p.second, "session left or icon hidden");
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
    std::vector<Client*> clients;
    std::map<DWORD, HANDLE> proxies;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        clients = g_clients;
        g_clients.clear();
        proxies = g_proxies;
        g_proxies.clear();
        g_retiring.clear();
    }
    for (Client* c : clients) {
        queue_line(c, "bye");
    }
    if (g_acceptor.joinable()) {
        g_acceptor.join();
    }
    if (g_supervisor.joinable()) {
        g_supervisor.join();
    }
    // Every proxy that is still up has been told to go away; give each one the
    // grace, then cut, then - only if it still will not - kill it.
    for (Client* c : clients) {
        cut_pipe(c, "host shutting down");
    }
    for (const auto& kv : proxies) {
        retire_proxy(kv.first, kv.second, "host shutting down");
    }
    for (Client* c : clients) {
        close_client(c);
    }
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
    if (line == last_sent) {
        return;
    }
    last_sent = line;
    std::vector<Client*> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        snapshot = g_clients;
    }
    for (Client* c : snapshot) {
        queue_line(c, line);
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
