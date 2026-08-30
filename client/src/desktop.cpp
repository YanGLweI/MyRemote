#include "desktop.hpp"

#include <sddl.h>
#include <tlhelp32.h>
#include <wtsapi32.h>

#include <algorithm>
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

bool console_session(DWORD* out, std::string* how, std::string* table) {
    // The station name is the only identifier that survives an RDP login: once
    // the console session is detached, WTSGetActiveConsoleSessionId() reports
    // 0xFFFFFFFF while "Console" still names the session owning the physical
    // display. Getting this wrong moves the host into somebody's RDP session.
    struct Row {
        DWORD id;
        std::wstring station;
        WTS_CONNECTSTATE_CLASS state;
    };
    std::vector<Row> rows;
    PWTS_SESSION_INFOW response = nullptr;
    DWORD count = 0;
    if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &response, &count)) {
        for (DWORD i = 0; i < count; ++i) {
            // pWinStationName points into the block WTSFreeMemory releases.
            rows.push_back({response[i].SessionId,
                            response[i].pWinStationName ? response[i].pWinStationName
                                                        : L"",
                            response[i].State});
        }
        WTSFreeMemory(response);
    }
    if (table) {
        for (const Row& r : rows) {
            char line[96];
            snprintf(line, sizeof(line), "%s%lu %ls(%d)", table->empty() ? "" : "; ",
                     r.id, r.station.c_str(), static_cast<int>(r.state));
            *table += line;
        }
        if (table->empty()) {
            *table = "no sessions enumerated";
        }
    }

    // An RDP station is never the physical console, whatever else claims it.
    auto is_remote = [&](DWORD id) {
        for (const Row& r : rows) {
            if (r.id == id) {
                return _wcsnicmp(r.station.c_str(), L"RDP-", 4) == 0;
            }
        }
        return false;
    };

    for (const Row& r : rows) {
        if (r.id != 0 && _wcsicmp(r.station.c_str(), L"Console") == 0) {
            *out = r.id;
            if (how) {
                *how = "console station";
            }
            return true;
        }
    }

    // No station literally named "Console": multi-session images (AVD, Windows
    // 365) have none at all, and neither does a failed enumeration.
    DWORD attached = WTSGetActiveConsoleSessionId();
    if (attached != 0 && attached != 0xFFFFFFFF && !is_remote(attached)) {
        *out = attached;
        if (how) {
            *how = "attached console";
        }
        return true;
    }

    // Last resort: LogonUI lives in the session owning a non-zero winlogon.exe.
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
                if (ProcessIdToSessionId(entry.th32ProcessID, &id) && id != 0 &&
                    !is_remote(id)) {
                    CloseHandle(snapshot);
                    *out = id;
                    if (how) {
                        *how = "winlogon owner";
                    }
                    return true;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    return false;
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

std::string input_desktop_name() {
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input) {
        return {};
    }
    wchar_t text[64] = {};
    DWORD needed = 0;
    std::string name;
    if (GetUserObjectInformationW(input, UOI_NAME, text, sizeof(text), &needed)) {
        name = wide_to_utf8(text);
    }
    CloseDesktop(input);
    return name;
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

std::vector<std::pair<int, int>> list_display_modes() {
    std::vector<std::pair<int, int>> modes;
    for (DWORD i = 0;; ++i) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(nullptr, i, &dm)) {
            break;
        }
        const auto mode = std::make_pair(static_cast<int>(dm.dmPelsWidth),
                                         static_cast<int>(dm.dmPelsHeight));
        if (std::find(modes.begin(), modes.end(), mode) == modes.end()) {
            modes.push_back(mode);
        }
    }
    std::sort(modes.begin(), modes.end(),
              [](const auto& a, const auto& b) {
                  return static_cast<long long>(a.first) * a.second >
                         static_cast<long long>(b.first) * b.second;
              });
    if (modes.size() > 64) {
        modes.resize(64);
    }
    return modes;
}

LONG change_display_mode(int width, int height) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    dm.dmPelsWidth = static_cast<DWORD>(width);
    dm.dmPelsHeight = static_cast<DWORD>(height);
    // CDS_TEST first: applying an untested mode can leave the console with a
    // black screen for the fifteen-second revert window.
    LONG result = ChangeDisplaySettingsExW(nullptr, &dm, nullptr, CDS_TEST, nullptr);
    if (result == DISP_CHANGE_SUCCESSFUL) {
        // Plain 0, never CDS_UPDATEREGISTRY: the operator asked for this
        // session's desktop, not for the machine's boot profile.
        result = ChangeDisplaySettingsExW(nullptr, &dm, nullptr, 0, nullptr);
    }
    return result;
}

}  // namespace win32util
