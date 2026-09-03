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
// How long a teardown waits for one of a client's own threads before it stops
// waiting for it. Every other timing constant above is M20's and stays as it is.
constexpr DWORD kCloseGraceMs = 1500;
// How long stop() waits for each proxy to actually hear the "bye". The gate is
// the completed-write counter below, not the queue: an empty queue only proves
// the writer took the word. A healthy proxy answers in milliseconds; the bound
// exists for the one that will not, and it is shared across all of them so two
// proxies cannot cost five seconds on an exit path that is already on a timer.
constexpr DWORD kByeConfirmMs = 2500;

struct Client {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD session = 0;
    // Kept for the teardown wait: the proxy map is keyed by session, while a
    // session can hold a dead client and its replacement at the same time. Only
    // a pid can say which process went away.
    DWORD client_pid = 0;
    std::atomic<bool> alive{true};
    std::atomic<bool> shutting_down{false};
    // Which door this client went out of. Until now all three producers just
    // cleared `alive`, so the one field the field has to read could not tell
    // "we cut this proxy" from "the proxy left by itself" - and those are the
    // two stories anybody debugging a missing icon needs to keep apart.
    enum Death : int { Living = 0, QueueOverflow, WriteFailed, FlushFailed, PipeGone, WeCutIt };
    std::atomic<int> death{Living};
    std::thread reader;
    std::thread writer;
    // std::thread's own handle cannot be waited on, so each thread signals this
    // pair on the way out; that is what makes a join bounded. (TrayIcon::stop()
    // solved the same problem the same way in M20.)
    HANDLE reader_done = nullptr;
    HANDLE writer_done = nullptr;
    // The writer duplicates its own thread handle here so a teardown can cancel
    // the synchronous WriteFile it may be parked inside. The pipe handle itself
    // is shared with the reader, so a handle-wide CancelIoEx would take the
    // reader's pending operation down with it.
    std::atomic<HANDLE> writer_thread{nullptr};
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<std::string> queue;
    // Two counters and the gap between them is the only honest answer to "did
    // the proxy hear it": a line that left the queue is invisible there while it
    // is still inside a blocking WriteFile.
    std::atomic<unsigned long long> words_said{0};
    std::atomic<unsigned long long> words_written{0};
    // Asked for only by stop(). Writing a line puts it in the pipe's buffer; the
    // disconnect that follows can discard that buffer before the proxy's reader
    // gets to it. A flush is what "the peer read it" actually means, and it is
    // issued on the writer thread so the same CancelSynchronousIo teardown that
    // releases a parked WriteFile releases it too.
    std::atomic<bool> flush_after_write{false};
    std::atomic<DWORD> since{0};             // when we accepted it
    std::atomic<DWORD> last_pong_tick{0};
    std::atomic<unsigned long long> last_pong_liveness{0};
    std::atomic<int> stale_pongs{0};
};

const char* death_name(int d) {
    switch (d) {
        case Client::QueueOverflow: return "queue overflow";
        case Client::WriteFailed: return "write failed";
        case Client::FlushFailed: return "peer never read it";
        case Client::PipeGone: return "pipe gone";
        case Client::WeCutIt: return "we cut it";
        default: return "still connected";
    }
}

// First reason wins. A write that fails and the broken pipe the reader trips
// over microseconds later are cause and consequence, and a log that named both
// would leave a reader guessing which one opened the door.
void mark_death(Client* c, int why) {
    int living = Client::Living;
    c->death.compare_exchange_strong(living, why);
}

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
            mark_death(c, Client::QueueOverflow);
            c->alive.store(false);
            return;
        }
        c->queue.push_back(line);
        c->words_said.fetch_add(1);
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
    // A handle to ourselves, so a teardown can cancel the synchronous WriteFile
    // this thread may be parked inside. THREAD_TERMINATE is what
    // CancelSynchronousIo asks for; the thread outlives the handle's use because
    // close_client() either joins it or abandons it outright.
    HANDLE self_thread = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &self_thread, THREAD_TERMINATE, FALSE, 0);
    c->writer_thread.store(self_thread);
    for (;;) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(c->qmu);
            c->qcv.wait(lock, [&] {
                return c->shutting_down.load() || !c->queue.empty();
            });
            if (c->queue.empty()) {
                if (c->shutting_down.load()) {
                    break;  // drained on the way out: everything was said
                }
                continue;
            }
            line = std::move(c->queue.front());
            c->queue.pop_front();
        }
        if (!raw_write(c->pipe, line)) {
            mark_death(c, Client::WriteFailed);
            c->alive.store(false);
            break;
        }
        // "Written" has to mean the peer read it, or the gate below is measuring
        // the buffer rather than the proxy.
        if (c->flush_after_write.load() && !FlushFileBuffers(c->pipe)) {
            mark_death(c, Client::FlushFailed);
            c->alive.store(false);
            break;
        }
        c->words_written.fetch_add(1);
    }
    SetEvent(c->writer_done);
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
        // The file is ours to write, so the verdict is ours to say: the proxy
        // must not tell its dialog "saved" on the strength of a pipe write.
        const bool ok =
            g_sink.save_config &&
            g_sink.save_config(line.substr(strlen("save_config ")));
        queue_line(self, ok ? "saved ok" : "saved fail");
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
            Sleep(10);
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
    // `alive` is what the reap gate keys off, so every exit clears it: a client
    // whose pipe we cut must still be collected. What the exit cannot tell on its
    // own is *why* - the same broken pipe means "we cut it" right after
    // cut_pipe() and "the proxy left" otherwise - so cut_pipe() records its own
    // reason and only an unasked-for exit lands as PipeGone here.
    if (!c->shutting_down.load()) {
        mark_death(c, Client::PipeGone);
    }
    c->alive.store(false);
    SetEvent(c->reader_done);
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
        c->client_pid = pid;
        ProcessIdToSessionId(pid, &c->session);
        const DWORD now = GetTickCount();
        c->since.store(now);
        c->last_pong_tick.store(now);
        // Both threads signal these on the way out. close_client() waits on them
        // rather than joining blind, because a join can only be bounded against
        // something waitable and std::thread's native handle is not.
        c->reader_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        c->writer_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
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
    // Quoted image path: lpApplicationName is null, so CreateProcess would
    // otherwise cut the command line at the first space of a path like
    // C:\Program Files\... and never find the exe at all.
    std::wstring cmd = L"\"" + g_exe + L"\" --tray-proxy --no-elevate";
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

// Wait for one of a client's threads to announce that it is done. `cancel`, when
// given, is a handle to that thread so its pending synchronous I/O - the only
// thing that can hold a WriteFile open against a peer that stopped reading - can
// be released. False means "still inside its loop": the caller must abandon it
// rather than keep waiting.
bool reap_thread(std::thread& t, HANDLE done, HANDLE cancel, const char* what) {
    if (!t.joinable()) {
        return true;  // never started, or already collected
    }
    if (done && WaitForSingleObject(done, kCloseGraceMs) == WAIT_OBJECT_0) {
        t.join();
        return true;
    }
    if (cancel) {
        mlog::warn(std::string("Tray proxies: the ") + what +
                   " is still inside a write; cancelling its synchronous I/O");
        CancelSynchronousIo(cancel);
        if (done && WaitForSingleObject(done, kCloseGraceMs) == WAIT_OBJECT_0) {
            t.join();
            return true;
        }
    }
    return false;
}

// Collect a client. The order is the whole of this function: break the pipe
// **before** waiting for the writer, because nothing else releases a thread
// parked in a synchronous WriteFile. Until now the overflow door reached these
// joins without anyone ever having called DisconnectNamedPipe - `cut_pipe` is
// skipped when `alive` is already false - so a proxy that stopped reading could
// hang the supervisor permanently. That is the worst outcome available here: the
// supervisor is the thing that repairs proxies, so every other icon on the
// machine goes quiet behind it and nobody brings them back.
void close_client(Client* c, const char* why) {
    c->shutting_down.store(true);
    c->qcv.notify_all();
    // Cancel before disconnect - the same order as cut_pipe(): a writer parked
    // in a synchronous WriteFile must be released before the pipe is broken,
    // because the disconnect itself can wait for that pending write.
    if (HANDLE t = c->writer_thread.load()) {
        CancelSynchronousIo(t);
    }
    DisconnectNamedPipe(c->pipe);  // idempotent with cut_pipe()
    const bool reader_out = reap_thread(c->reader, c->reader_done, nullptr, "reader");
    const bool writer_out =
        reap_thread(c->writer, c->writer_done, c->writer_thread.load(), "writer");
    mlog::info(std::string("Tray proxies: session ") + std::to_string(c->session) +
               " closed (reason=" + death_name(c->death.load()) + ", " + why + ")");
    if (reader_out && writer_out) {
        CloseHandle(c->pipe);
        c->pipe = INVALID_HANDLE_VALUE;
        CloseHandle(c->reader_done);
        CloseHandle(c->writer_done);
        c->reader_done = c->writer_done = nullptr;
        delete c;
        return;
    }
    // The last layer, and the one that makes "a teardown cannot be hung" true
    // even if neither the disconnect nor the cancel is what frees the write:
    // leak the Client instead of freeing an object a live thread is still using.
    // Its pipe handle leaks with it on purpose - closing a handle another
    // thread may still be writing through is the exact bug this file is about.
    mlog::error("Tray proxies: a thread of the session " +
                std::to_string(c->session) +
                " proxy would not come back; it was abandoned, not joined");
    if (c->reader.joinable()) {
        c->reader.detach();
    }
    if (c->writer.joinable()) {
        c->writer.detach();
    }
}

// Cut the pipe instead of killing the process: the proxy's own read loop turns
// the broken pipe into a clean exit, and a clean exit is the only moment it can
// delete its icon.
void cut_pipe(Client* c, const char* why) {
    mlog::info(std::string("Tray proxies: cutting the pipe to session ") +
               std::to_string(c->session) + " (" + why + ")");
    mark_death(c, Client::WeCutIt);
    c->shutting_down.store(true);
    c->qcv.notify_all();
    // Cancel before disconnect: DisconnectNamedPipe can wait for a synchronous
    // WriteFile that is parked against a peer which stopped reading - the very
    // wedge this file was written to make impossible. The cancel releases the
    // write first, so the disconnect below returns at once.
    if (HANDLE t = c->writer_thread.load()) {
        CancelSynchronousIo(t);
    }
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
        // Snapshot under the lock, talk outside it - the same shape as
        // broadcast_state, and the only way the promise on queue_line ("a proxy
        // that stopped draining holds up nothing but its own queue") is true.
        std::vector<Client*> to_ping;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (static_cast<int32_t>(now - g_last_ping_tick) >= (int)kPingEveryMs) {
                g_last_ping_tick = now;
                to_ping = g_clients;
            }
        }
        for (Client* c : to_ping) {
            queue_line(c, "ping");
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
            close_client(c, "supervisor reaped it");
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
        // Ask before speaking: the bye below is the word whose delivery this gate
        // exists to prove. Measured 2026-09-03 with the flush absent - the client
        // on the other end read "bye" in 108ms while the real proxy still logged
        // "host pipe closed", because WriteFile only promises the buffer.
        c->flush_after_write.store(true);
        queue_line(c, "bye");
    }
    // The words have to reach the wire before the pipe is cut. A proxy that
    // hears "bye" logs "host said bye"; the same proxy that only sees the pipe
    // break logs "host pipe closed" - two different stories for one healthy
    // exit, and the field has been recording the wrong one.
    std::vector<unsigned long long> target;
    target.reserve(clients.size());
    for (Client* c : clients) {
        target.push_back(c->words_said.load());
    }
    std::vector<HANDLE> process(clients.size(), nullptr);
    for (size_t i = 0; i < clients.size(); ++i) {
        if (clients[i]->client_pid) {
            process[i] = OpenProcess(
                SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                clients[i]->client_pid);
        }
    }
    const ULONGLONG bye_deadline = GetTickCount64() + kByeConfirmMs;
    for (;;) {
        bool all_settled = true;
        for (size_t i = 0; i < clients.size(); ++i) {
            Client* c = clients[i];
            if (c->words_written.load() >= target[i]) {
                continue;  // said and heard
            }
            if (!c->alive.load()) {
                continue;  // this door is shut; nothing more will ever be said
            }
            if (process[i] &&
                WaitForSingleObject(process[i], 0) == WAIT_OBJECT_0) {
                continue;  // the proxy is gone; there is nobody left to tell
            }
            all_settled = false;
        }
        if (all_settled || GetTickCount64() >= bye_deadline) {
            break;
        }
        Sleep(20);
    }
    for (size_t i = 0; i < clients.size(); ++i) {
        if (process[i]) {
            CloseHandle(process[i]);
        }
        Client* c = clients[i];
        if (c->words_written.load() < target[i]) {
            mlog::warn("Tray proxies: session " + std::to_string(c->session) +
                       " bye unconfirmed at " +
                       std::to_string(static_cast<unsigned long long>(kByeConfirmMs)) +
                       "ms (" + death_name(c->death.load()) +
                       "); its tray log will read \"host pipe closed\"");
        }
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
        close_client(c, "host shutting down");
    }
    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
}

// The proxy reads this line by the first "key=" it finds, and name is the last
// field precisely because it may contain spaces. A device name that happens to
// contain `tray_icon=0` would therefore be read as a command - and "hide the
// icon" is a decision the name must never be able to make. Newlines would break
// the framing outright. Spaces stay.
std::string safe_name_for_state(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20) {
            continue;
        }
        out.push_back(c == '=' ? '-' : c);
    }
    return out;
}

void broadcast_state(bool paused, bool registered, bool tray_icon,
                     const std::string& server, const std::string& name) {
    bool began_hiding = false;
    bool began_showing = false;
    size_t sessions = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        sessions = g_clients.size();
        began_hiding = g_tray_enabled && !tray_icon && sessions != 0;
        began_showing = !g_tray_enabled && tray_icon;
        g_state_paused.store(paused);
        g_state_registered.store(registered);
        g_tray_enabled = tray_icon;
        g_state_server = server;
        g_state_name = safe_name_for_state(name);
    }
    if (began_hiding) {
        // The one line that explains a machine whose icons went away.
        mlog::info("Tray proxies: the setting now says no icon; retiring " +
                   std::to_string(sessions) + " proxy session(s)");
    } else if (began_showing) {
        mlog::info("Tray proxies: the icon is wanted again; sessions get a proxy");
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
