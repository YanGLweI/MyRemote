#include "service.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "desktop.hpp"
#include "log.hpp"

namespace svc {

const wchar_t* const kServiceName = L"MyRemoteAgent";
const wchar_t* const kDisplayName = L"MyRemote Control Agent";

namespace {

constexpr wchar_t kHostParams[] = L" --session-host --background --no-elevate";
// Written out rather than SE_TCB_NAME: that macro expands to a narrow string
// unless UNICODE is defined, and every call here is the explicit W variant.
constexpr wchar_t kSeTcbName[] = L"SeTcbPrivilege";
constexpr wchar_t kSeDebugName[] = L"SeDebugPrivilege";

// One host at a time: two of them would mean two tunnels registering the same
// device id, and the controller retires the older one on every registration.
HANDLE g_host = nullptr;
DWORD g_host_session = 0xFFFFFFFF;
DWORD g_host_pid = 0;
SERVICE_STATUS_HANDLE g_status_handle = nullptr;
HANDLE g_stop_event = nullptr;    // manual reset: stay set once stopping
HANDLE g_wakeup_event = nullptr;  // auto reset: session change, shutdown

class ServiceHandle {
public:
    explicit ServiceHandle(SC_HANDLE handle = nullptr) : handle_(handle) {}
    ~ServiceHandle() {
        if (handle_) {
            CloseServiceHandle(handle_);
        }
    }
    ServiceHandle(const ServiceHandle&) = delete;
    ServiceHandle& operator=(const ServiceHandle&) = delete;
    ServiceHandle(ServiceHandle&& other) : handle_(other.release()) {}
    ServiceHandle& operator=(ServiceHandle&& other) {
        if (this != &other) {
            if (handle_) {
                CloseServiceHandle(handle_);
            }
            handle_ = other.release();
        }
        return *this;
    }

    SC_HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    SC_HANDLE release() {
        SC_HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    SC_HANDLE handle_ = nullptr;
};

void report_status(DWORD state, DWORD checkpoint, DWORD wait_hint) {
    if (!g_status_handle) {
        return;
    }
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = NO_ERROR;
    status.dwServiceSpecificExitCode = 0;
    status.dwCheckPoint = checkpoint;
    status.dwWaitHint = wait_hint;
    status.dwControlsAccepted =
        state == SERVICE_RUNNING
            ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN |
               SERVICE_ACCEPT_SESSIONCHANGE)
            : 0;
    SetServiceStatus(g_status_handle, &status);
}

std::string last_error(const char* what) {
    return std::string(what) + ", error " + std::to_string(GetLastError());
}

// SYSTEM token, retargeted at the console session, primary. Deliberately not
// WTSQueryUserToken: that one returns ERROR_NO_TOKEN in exactly the state this
// feature exists for (nobody logged in), and a SYSTEM host outranks every
// elevated window in the session, which is what makes UAC driveable.
HANDLE launch_host(DWORD session) {
    std::wstring exe = win32util::exe_path_wide();
    if (exe.empty()) {
        mlog::error("Cannot resolve own executable path");
        return nullptr;
    }
    HANDLE token = nullptr;
    HANDLE primary = nullptr;
    HANDLE process = nullptr;
    do {
        if (!OpenProcessToken(GetCurrentProcess(),
                              TOKEN_DUPLICATE | TOKEN_QUERY |
                                  TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT |
                                  TOKEN_ADJUST_SESSIONID |
                                  TOKEN_ADJUST_PRIVILEGES,
                              &token)) {
            mlog::error(last_error("OpenProcessToken failed"));
            break;
        }
        if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr,
                              SecurityImpersonation, TokenPrimary, &primary)) {
            mlog::error(last_error("DuplicateTokenEx failed"));
            break;
        }
        DWORD sid = session;
        if (!SetTokenInformation(primary, TokenSessionId, &sid, sizeof(sid))) {
            mlog::error(last_error(
                "SetTokenInformation(TokenSessionId) failed; needs SeTcbPrivilege"));
            break;
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        // Never "Winsta0\\Winlogon": that desktop does not exist once someone
        // is logged in, and CreateProcessAsUser reports the unfriendly
        // ERROR_FILE_NOT_FOUND when it cannot open the one it was given. The
        // host follows the real input desktop itself.
        si.lpDesktop = const_cast<LPWSTR>(L"Winsta0\\Default");
        PROCESS_INFORMATION pi{};
        std::wstring command = L"\"" + exe + L"\"" + kHostParams;
        if (!CreateProcessAsUserW(primary, exe.c_str(), command.data(), nullptr,
                                  nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            mlog::error(last_error("CreateProcessAsUserW failed"));
            break;
        }
        CloseHandle(pi.hThread);
        process = pi.hProcess;
        g_host_pid = pi.dwProcessId;
        mlog::info("Launched session host pid=" + std::to_string(pi.dwProcessId) +
                   " in session " + std::to_string(session));
    } while (false);
    if (primary) {
        CloseHandle(primary);
    }
    if (token) {
        CloseHandle(token);
    }
    return process;
}

void shutdown_host() {
    if (!g_host) {
        return;
    }
    if (WaitForSingleObject(g_host, 0) == WAIT_TIMEOUT) {
        TerminateProcess(g_host, 0);
        WaitForSingleObject(g_host, 2000);
    }
    CloseHandle(g_host);
    g_host = nullptr;
    g_host_session = 0xFFFFFFFF;
}

// How long a console handover has to look stable before we act on it, and how
// young a host has to be for us to leave it alone. A session-change storm
// (mstsc connecting fires five of them inside two seconds) must never be able
// to terminate a healthy host: that kills the only outbound tunnel. The settle
// no longer has to be long, only longer than the storm itself, because
// console_session() can name nothing but the physical console.
constexpr ULONGLONG kSettleMs = 2000;
constexpr ULONGLONG kMinHostAgeMs = 15000;

void supervise() {
    ULONGLONG last_launch_ms = 0;
    int fast_failures = 0;
    DWORD backoff_ms = 2000;
    DWORD pending = 0xFFFFFFFF;
    ULONGLONG pending_ms = 0;

    while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
        DWORD want = 0xFFFFFFFF;
        std::string how;
        bool resolved = win32util::console_session(&want, &how);
        bool host_dead = g_host && WaitForSingleObject(g_host, 0) == WAIT_OBJECT_0;
        ULONGLONG now = GetTickCount64();

        if (g_host && host_dead) {
            DWORD code = 0;
            GetExitCodeProcess(g_host, &code);
            mlog::info("Session host exited with code " + std::to_string(code));
            shutdown_host();
        } else if (g_host && resolved && want != g_host_session) {
            // Kept across iterations and deliberately not re-armed by a wake-up,
            // so consecutive SESSIONCHANGE events cannot shorten the settle.
            if (want != pending) {
                pending = want;
                pending_ms = now;
            } else if (now - pending_ms >= kSettleMs &&
                       now - last_launch_ms >= kMinHostAgeMs) {
                mlog::info("Console session moved to " + std::to_string(want) +
                           " (" + how + "); restarting the host");
                TerminateProcess(g_host, 0);
                pending = 0xFFFFFFFF;
                shutdown_host();
            }
        } else {
            pending = 0xFFFFFFFF;
        }

        if (!g_host && host_dead) {
            if (now - last_launch_ms < kMinHostAgeMs) {
                ++fast_failures;
            } else {
                fast_failures = 0;
            }
            backoff_ms =
                fast_failures >= 4 ? 30000u : (2000u << fast_failures);
            if (fast_failures >= 5) {
                mlog::error("Session host keeps dying; retrying every 30s - check "
                            "agent.log for an UNHANDLED EXCEPTION");
            }
        }

        if (!g_host && resolved && want != 0) {
            g_host = launch_host(want);
            g_host_session = want;
            last_launch_ms = GetTickCount64();
            if (!g_host) {
                // CreateProcessAsUser fails as ERROR_FILE_NOT_FOUND when the
                // desktop it was told to start on cannot be opened.
                DWORD err = GetLastError();
                mlog::error("Launching a session host in session " +
                            std::to_string(want) + " failed (last error " +
                            std::to_string(err) + "); retrying");
            }
        }

        HANDLE waits[3] = {g_stop_event, g_wakeup_event, g_host};
        DWORD count = g_host ? 3 : 2;
        WaitForMultipleObjects(count, waits, FALSE, backoff_ms);
    }
    shutdown_host();
}

DWORD WINAPI ctrl_handler(DWORD control, DWORD event_type, LPVOID event_data,
                          LPVOID) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
            mlog::info("Service stop requested");
            SetEvent(g_stop_event);
            SetEvent(g_wakeup_event);
            report_status(SERVICE_STOP_PENDING, 1, 5000);
            return NO_ERROR;
        case SERVICE_CONTROL_PRESHUTDOWN:
            mlog::info("System shutdown; releasing the session host");
            SetEvent(g_stop_event);
            SetEvent(g_wakeup_event);
            return NO_ERROR;
        case SERVICE_CONTROL_SESSIONCHANGE: {
            DWORD session = 0xFFFFFFFF;
            if (event_data) {
                session = static_cast<WTSSESSION_NOTIFICATION*>(event_data)
                              ->dwSessionId;
            }
            mlog::info("Session change event " + std::to_string(event_type) +
                       " session " + std::to_string(session));
            // Only these can move the console. Logon, lock and unlock are
            // handled inside the host by its desktop follower; waking the
            // supervisor for them just fed the restart storm.
            const bool may_move_console =
                event_type == WTS_CONSOLE_CONNECT ||
                event_type == WTS_CONSOLE_DISCONNECT ||
                event_type == WTS_REMOTE_CONNECT ||
                event_type == WTS_REMOTE_DISCONNECT ||
                event_type == WTS_SESSION_LOGOFF;
            if (may_move_console) {
                SetEvent(g_wakeup_event);
            }
            return NO_ERROR;
        }
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

VOID WINAPI service_main(DWORD, LPWSTR*) {
    g_status_handle =
        RegisterServiceCtrlHandlerExW(kServiceName, ctrl_handler, nullptr);
    if (!g_status_handle) {
        return;
    }
    report_status(SERVICE_START_PENDING, 1, 20000);

    std::wstring dir = win32util::program_data_dir();
    if (dir.empty()) {
        dir = win32util::utf8_to_wide(win32util::exe_dir());
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    mlog::init(win32util::wide_to_utf8((dir + L"\\service.log").c_str()));
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* info) -> LONG {
        char line[160];
        snprintf(line, sizeof(line),
                 "UNHANDLED EXCEPTION code=0x%08lX at %p",
                 static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
                 info->ExceptionRecord->ExceptionAddress);
        mlog::error(line);
        return EXCEPTION_EXECUTE_HANDLER;
    });
    mlog::info("MyRemote agent service starting");
    // SeTcbPrivilege is what lets this process move a token into another
    // session; SeDebugPrivilege is what lets it retire a stray agent.
    mlog::info(std::string("SeTcbPrivilege ") +
               (win32util::enable_privilege(kSeTcbName) ? "enabled" : "unavailable"));
    mlog::info(std::string("SeDebugPrivilege ") +
               (win32util::enable_privilege(kSeDebugName) ? "enabled" : "unavailable"));

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wakeup_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_stop_event || !g_wakeup_event) {
        report_status(SERVICE_STOPPED, 0, 0);
        return;
    }
    report_status(SERVICE_RUNNING, 0, 0);

    supervise();

    mlog::info("MyRemote agent service stopped");
    report_status(SERVICE_STOPPED, 0, 0);
}

bool wait_for_state(SC_HANDLE handle, DWORD target, DWORD timeout_ms,
                    std::wstring* why) {
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    SERVICE_STATUS status{};
    while (GetTickCount64() < deadline) {
        if (!QueryServiceStatus(handle, &status)) {
            if (why) {
                *why = L"QueryServiceStatus failed";
            }
            return false;
        }
        if (status.dwCurrentState == target) {
            return true;
        }
        Sleep(200);
    }
    if (why) {
        *why = L"service did not reach the requested state in time";
    }
    return false;
}

// ControlService always marshals its SERVICE_STATUS out parameter: passing
// nullptr fails the call with ERROR_INVALID_ADDRESS and the service is never
// stopped. Returns the error code so callers can accept the ones that mean
// "already stopped".
DWORD request_stop(SC_HANDLE service) {
    SERVICE_STATUS status{};
    return ControlService(service, SERVICE_CONTROL_STOP, &status) ? ERROR_SUCCESS
                                                                  : GetLastError();
}

void apply_service_config(SC_HANDLE handle) {
    SERVICE_DESCRIPTIONW description{};
    description.lpDescription = const_cast<LPWSTR>(
        L"MyRemote Control Agent - provides remote desktop and remote logon on the "
        L"logon screen and in every session.");
    ChangeServiceConfig2W(handle, SERVICE_CONFIG_DESCRIPTION, &description);

    // Not delayed: the delay costs 1-2 minutes of an unreachable machine
    // right after power-on, which is exactly when a remote logon is needed.
    // Started before the network is up, the host just retries from its 1s
    // backoff. Written as FALSE rather than omitted because the flag persists
    // on an already-registered service.
    SERVICE_DELAYED_AUTO_START_INFO delayed{};
    delayed.fDelayedAutostart = FALSE;
    ChangeServiceConfig2W(handle, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                          &delayed);

    // A boot-time dial-out must not be on the critical path, and a host that
    // dies has to come back without anyone logging in.
    SERVICE_FAILURE_ACTIONSW actions{};
    SC_ACTION steps[3] = {{SC_ACTION_RESTART, 5000},
                          {SC_ACTION_RESTART, 5000},
                          {SC_ACTION_RESTART, 60000}};
    actions.dwResetPeriod = 86400;
    actions.cActions = 3;
    actions.lpsaActions = steps;
    ChangeServiceConfig2W(handle, SERVICE_CONFIG_FAILURE_ACTIONS, &actions);

    // The 5s default shutdown is not enough to release the console host
    // cleanly, and a half-dead host keeps holding the device row open.
    SERVICE_PRESHUTDOWN_INFO preshutdown{};
    preshutdown.dwPreshutdownTimeout = 10000;
    ChangeServiceConfig2W(handle, SERVICE_CONFIG_PRESHUTDOWN_INFO, &preshutdown);
}

// The config has to be writable by the interactive user who configures the
// machine, and by SYSTEM which reads it from another session.
void ensure_shared_directory() {
    std::wstring dir = win32util::program_data_dir();
    if (dir.empty()) {
        return;
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    win32util::run_command(L"icacls \"" + dir +
                           L"\" /grant \"*S-1-5-18:(OI)(CI)F\" "
                           L"\"*S-1-5-32-544:(OI)(CI)F\" \"*S-1-5-4:(OI)(CI)MW\"");
}

SC_HANDLE open_registered(DWORD access, std::wstring* why) {
    ServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        if (why) {
            *why = L"cannot open the service control manager";
        }
        return nullptr;
    }
    SC_HANDLE handle = OpenServiceW(scm.get(), kServiceName, access);
    if (!handle && why) {
        *why = L"MyRemoteAgent service is not installed";
    }
    return handle;
}

}  // namespace

int run_as_service() {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), service_main}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        DWORD code = GetLastError();
        if (code == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            printf("agent.exe was not started by the service control manager "
                   "(error %lu). Install it with: agent.exe --install-service\n",
                   static_cast<unsigned long>(code));
            return 1;
        }
        printf("StartServiceCtrlDispatcher failed: error %lu\n",
               static_cast<unsigned long>(code));
        return 1;
    }
    return 0;
}

bool install_or_update(std::wstring* why) {
    std::wstring exe = win32util::exe_path_wide();
    if (exe.empty()) {
        if (why) {
            *why = L"cannot resolve own executable path";
        }
        return false;
    }
    std::wstring bin_path = L"\"" + exe + L"\" --service";

    ServiceHandle scm(OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!scm) {
        if (why) {
            *why = L"administrator rights are required to create a service";
        }
        return false;
    }

    ServiceHandle service(CreateServiceW(
        scm.get(), kServiceName, kDisplayName, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        bin_path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
    if (!service) {
        if (GetLastError() != ERROR_SERVICE_EXISTS) {
            if (why) {
                *why = L"CreateService failed";
            }
            return false;
        }
        // Re-running the installer is the supported way to move the service to
        // a new build, so re-pointing binPath has to be enough.
        service = ServiceHandle(OpenServiceW(scm.get(), kServiceName,
                                             SERVICE_ALL_ACCESS));
        if (!service) {
            if (why) {
                *why = L"the service exists but cannot be opened";
            }
            return false;
        }
        if (!ChangeServiceConfigW(service.get(), SERVICE_NO_CHANGE,
                                  SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                  bin_path.c_str(), nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr)) {
            if (why) {
                *why = L"ChangeServiceConfig failed";
            }
            return false;
        }
    }

    apply_service_config(service.get());
    ensure_shared_directory();

    if (!StartServiceW(service.get(), 0, nullptr) &&
        GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        if (why) {
            *why = L"the service is installed but could not be started";
        }
        return false;
    }
    // Two agents on one machine fight over the same device id; the logon task
    // is redundant now that the service starts before anyone logs in.
    if (win32util::set_autostart(false)) {
        mlog::info("Legacy logon task removed during service install");
    }
    return true;
}

bool uninstall(std::wstring* why) {
    ServiceHandle service(open_registered(
        SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE, why));
    if (!service) {
        return false;
    }
    request_stop(service.get());
    wait_for_state(service.get(), SERVICE_STOPPED, 5000, nullptr);
    if (!DeleteService(service.get())) {
        if (why) {
            *why = L"DeleteService failed; is the service still running?";
        }
        return false;
    }
    return true;
}

bool start(std::wstring* why) {
    ServiceHandle service(open_registered(SERVICE_START | SERVICE_QUERY_STATUS,
                                          why));
    if (!service) {
        return false;
    }
    if (!StartServiceW(service.get(), 0, nullptr) &&
        GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        if (why) {
            *why = L"StartService failed";
        }
        return false;
    }
    return wait_for_state(service.get(), SERVICE_RUNNING, 10000, why);
}

bool stop(std::wstring* why) {
    ServiceHandle service(open_registered(SERVICE_STOP | SERVICE_QUERY_STATUS,
                                          why));
    if (!service) {
        return false;
    }
    const DWORD err = request_stop(service.get());
    if (err != ERROR_SUCCESS && err != ERROR_SERVICE_NOT_ACTIVE) {
        if (why) {
            // 5 = not allowed to stop it, 1052 = STOP is not accepted.
            *why = L"ControlService(STOP) failed, error " + std::to_wstring(err);
        }
        return false;
    }
    return wait_for_state(service.get(), SERVICE_STOPPED, 10000, why);
}

bool is_installed() {
    ServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        return false;
    }
    ServiceHandle service(
        OpenServiceW(scm.get(), kServiceName, SERVICE_QUERY_CONFIG));
    return static_cast<bool>(service);
}

std::string query() {
    std::string out;
    ServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    ServiceHandle service(
        scm ? OpenServiceW(scm.get(), kServiceName,
                           SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG)
            : nullptr);
    if (!service) {
        return "MyRemoteAgent: not installed\n";
    }
    SERVICE_STATUS status{};
    if (QueryServiceStatus(service.get(), &status)) {
        const char* state = "unknown";
        switch (status.dwCurrentState) {
            case SERVICE_STOPPED: state = "STOPPED"; break;
            case SERVICE_START_PENDING: state = "START_PENDING"; break;
            case SERVICE_STOP_PENDING: state = "STOP_PENDING"; break;
            case SERVICE_RUNNING: state = "RUNNING"; break;
            default: break;
        }
        out += std::string("MyRemoteAgent: ") + state + "\n";
    }
    DWORD needed = 0;
    QueryServiceConfigW(service.get(), nullptr, 0, &needed);
    std::vector<unsigned char> buffer(needed);
    if (needed &&
        QueryServiceConfigW(service.get(),
                            reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(
                                buffer.data()), needed, &needed)) {
        auto* config = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.data());
        out += "  binPath: " + win32util::wide_to_utf8(config->lpBinaryPathName) + "\n";
        out += "  start type: " +
               std::string(config->dwStartType == SERVICE_AUTO_START
                               ? "auto"
                               : std::to_string(config->dwStartType)) +
               "\n";
    }
    DWORD console = 0xFFFFFFFF;
    std::string how;
    std::string stations;
    if (win32util::console_session(&console, &how, &stations)) {
        out += "  console session: " + std::to_string(console) + " (" + how + ")\n";
    } else {
        out += "  console session: unresolved - the host stays where it is\n";
    }
    out += "  stations: " + stations + "\n";
    // Same resolution order the host itself used, otherwise the status file is
    // looked for in the wrong place on a build-tree install.
    std::string log_dir = win32util::resolve_paths(std::string()).log_dir;
    if (log_dir.empty()) {
        return out;
    }
    std::wstring status_file = win32util::utf8_to_wide(log_dir + "\\host.status");
    std::string text;
    if (HANDLE file = CreateFileW(status_file.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        file != INVALID_HANDLE_VALUE) {
        char chunk[256] = {};
        DWORD read = 0;
        while (ReadFile(file, chunk, sizeof(chunk) - 1, &read, nullptr) && read) {
            text.append(chunk, read);
        }
        CloseHandle(file);
        out += "  host: " + text + "\n";
    } else {
        out += "  host: no session host running\n";
    }
    return out;
}

}  // namespace svc
