#include "desktop.hpp"

#include <sddl.h>
#include <tlhelp32.h>
#include <wtsapi32.h>

#include <cstdio>
#include <string>
#include <vector>

#include "log.hpp"

namespace win32util {

namespace {

constexpr wchar_t kAppFolder[] = L"MyRemote";

}  // namespace

void attach_parent_console() {
    // A GUI-subsystem exe usually arrives with no standard handles at all, so
    // borrow the console it was started from. If the caller already handed us
    // one - including a redirected file or pipe - writing there is the whole
    // point, and reopening CONOUT$ would throw that redirect away.
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

bool enable_privilege(LPCWSTR name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          &token)) {
        mlog::warn("OpenProcessToken failed; privilege " +
                   wide_to_utf8(name) + " not enabled");
        return false;
    }
    LUID luid{};
    bool ok = LookupPrivilegeValueW(nullptr, name, &luid) != FALSE;
    if (ok) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr,
                                   nullptr) != FALSE &&
             GetLastError() != ERROR_NOT_ALL_ASSIGNED;
    }
    CloseHandle(token);
    return ok;
}

bool process_is_system() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<unsigned char> buffer(needed);
    bool system = false;
    if (!buffer.empty() &&
        GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) {
        LPWSTR sid_text = nullptr;
        if (ConvertSidToStringSidW(
                reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,
                &sid_text)) {
            system = wcscmp(sid_text, L"S-1-5-18") == 0;
            LocalFree(sid_text);
        }
    }
    CloseHandle(token);
    return system;
}

std::string wide_to_utf8(const wchar_t* w) {
    if (!w || !*w) {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(len) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                  nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(),
                        len);
    return w;
}

std::wstring exe_path_wide() {
    wchar_t path[MAX_PATH * 4] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (n == 0 || n >= ARRAYSIZE(path)) {
        return {};
    }
    return std::wstring(path, n);
}

std::string exe_dir() {
    std::wstring full = exe_path_wide();
    size_t pos = full.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return ".";
    }
    return wide_to_utf8(full.substr(0, pos).c_str());
}

std::wstring program_data_dir() {
    wchar_t root[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", root, ARRAYSIZE(root));
    if (n == 0 || n >= ARRAYSIZE(root)) {
        return {};
    }
    return std::wstring(root, n) + L"\\" + kAppFolder;
}

AgentPaths resolve_paths(const std::string& cli_override) {
    AgentPaths paths;
    if (!cli_override.empty()) {
        std::wstring wide = utf8_to_wide(cli_override);
        size_t pos = wide.find_last_of(L"\\/");
        paths.config = cli_override;
        paths.log_dir = pos == std::wstring::npos
                            ? "."
                            : wide_to_utf8(wide.substr(0, pos).c_str());
        return paths;
    }

    std::wstring shared = program_data_dir();
    if (!shared.empty()) {
        std::wstring shared_config = shared + L"\\config.json";
        if (GetFileAttributesW(shared_config.c_str()) != INVALID_FILE_ATTRIBUTES) {
            paths.config = wide_to_utf8(shared_config.c_str());
            paths.log_dir = wide_to_utf8(shared.c_str());
            return paths;
        }
    }
    // Development layout: agent.exe sitting in the build tree next to the
    // config.json it was configured with.
    std::string dir = exe_dir();
    std::string local = dir + "\\config.json";
    if (GetFileAttributesW(utf8_to_wide(local).c_str()) != INVALID_FILE_ATTRIBUTES) {
        paths.config = local;
        paths.log_dir = dir;
        return paths;
    }
    if (!shared.empty()) {
        CreateDirectoryW(shared.c_str(), nullptr);
        paths.config = wide_to_utf8((shared + L"\\config.json").c_str());
        paths.log_dir = wide_to_utf8(shared.c_str());
        return paths;
    }
    paths.config = local;
    paths.log_dir = dir;
    return paths;
}

DWORD resolve_console_session(bool verbose) {
    DWORD session = WTSGetActiveConsoleSessionId();
    if (verbose) {
        mlog::info("WTSGetActiveConsoleSessionId -> " + std::to_string(session));
    }
    if (session != 0xFFFFFFFF && session != 0) {
        return session;
    }

    // The console id alone is not enough on every SKU: fast user switching and
    // detached sessions leave it at 0 while a live session clearly owns the
    // desktop, so fall back to enumerating.
    PWTS_SESSION_INFOW response = nullptr;
    DWORD count = 0;
    DWORD best_active = 0xFFFFFFFF;
    DWORD best_connected = 0xFFFFFFFF;
    if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &response, &count)) {
        for (DWORD i = 0; i < count; ++i) {
            DWORD id = response[i].SessionId;
            if (id == 0) {
                continue;  // session 0 is services, never the interactive desktop
            }
            if (response[i].State == WTSActive && id > best_active) {
                best_active = id;
            } else if (response[i].State == WTSConnected && id > best_connected) {
                best_connected = id;
            }
        }
        WTSFreeMemory(response);
    }
    if (verbose) {
        mlog::info("enumerated sessions: active=" + std::to_string(best_active) +
                   " connected=" + std::to_string(best_connected));
    }
    if (best_active != 0xFFFFFFFF) {
        return best_active;
    }
    if (best_connected != 0xFFFFFFFF) {
        return best_connected;
    }

    // Nothing reports itself as active: the machine is at the logon screen, and
    // the session hosting LogonUI is the one owning a non-zero winlogon.exe.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, L"winlogon.exe") != 0) {
                    continue;
                }
                DWORD id = 0;
                if (ProcessIdToSessionId(entry.th32ProcessID, &id) && id != 0) {
                    CloseHandle(snapshot);
                    if (verbose) {
                        mlog::info("winlogon.exe pid=" +
                                   std::to_string(entry.th32ProcessID) + " owns session " +
                                   std::to_string(id));
                    }
                    return id;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    return session;
}

const wchar_t* const kAutostartTaskName = L"MyRemote Agent";

int run_command(const std::wstring& command_line) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring mutable_line = command_line;
    if (!CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = static_cast<DWORD>(-1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

// Logon-time autostart must run elevated: a Run-key agent starts with a
// filtered token and can then never drive elevated windows remotely.
bool set_autostart(bool enable) {
    std::wstring path = exe_path_wide();
    std::wstring action = enable
                              ? L" /Create /F /SC ONLOGON /RL HIGHEST /TR \"\\\"" +
                                    path + L"\\\" --background\\\"\""
                              : L" /Delete /F";
    int code = run_command(L"schtasks" + action + L" /TN \"" +
                           kAutostartTaskName + L"\"");

    // Retire the legacy Run key so it cannot start a second, limited agent.
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, L"MyRemoteAgent");
        RegCloseKey(key);
    }
    return code == 0;
}

DesktopFollower::~DesktopFollower() {
    if (held_) {
        CloseDesktop(held_);
    }
}

bool DesktopFollower::update(std::string* name, bool* changed) {
    if (changed) {
        *changed = false;
    }
    HDESK input = OpenInputDesktop(0, FALSE, MAXIMUM_ALLOWED);
    if (!input) {
        if (!logged_denial_ && GetLastError() == ERROR_ACCESS_DENIED) {
            logged_denial_ = true;
            mlog::warn("OpenInputDesktop denied: following the logon screen needs "
                       "SeTcbPrivilege, i.e. a service-hosted agent");
        }
        return false;
    }
    wchar_t text[64] = {};
    DWORD needed = 0;
    bool named = GetUserObjectInformationW(input, UOI_NAME, text, sizeof(text),
                                          &needed) != FALSE;
    std::string observed = named ? wide_to_utf8(text) : std::string();
    bool attached = true;
    if (named && observed != name_) {
        // Never SwitchDesktop(): that would yank the physical monitor away from
        // whoever is sitting at the machine. Only move this thread.
        if (SetThreadDesktop(input)) {
            if (held_) {
                CloseDesktop(held_);
            }
            held_ = input;  // ours while the thread stays on it
            name_ = observed;
            on_secure_desktop_ = name_ != "Default";
            if (changed) {
                *changed = true;
            }
            if (name) {
                *name = name_;
            }
            return true;
        }
        // A window, a hook or a GDI object from the previous desktop pins this
        // thread down; the next poll retries once the caller has dropped them.
        attached = false;
    }
    CloseDesktop(input);
    if (name) {
        *name = name_;
    }
    return attached;
}

}  // namespace win32util
