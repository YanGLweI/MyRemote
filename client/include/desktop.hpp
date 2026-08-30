#pragma once

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

// Shared Win32 plumbing for the two M14 process roles: the service (session 0,
// supervisor only) and the console-session host (capture + input + tunnel).
// Nothing in here may open a socket: keeping the tunnel in one process is what
// guarantees one device row per machine.
namespace win32util {

// A GUI-subsystem exe has no console, so every CLI branch has to steal the
// parent's to say anything at all.
void attach_parent_console();

// Adds one privilege to this process. Returns false when the privilege is held
// but not assignable (ERROR_NOT_ALL_ASSIGNED), which is the common
// "running as admin but not SYSTEM" case.
bool enable_privilege(LPCWSTR name);

bool process_is_system();

// UTF-8 is the string encoding used across the codebase; the Win32 file APIs
// are wide, so paths cross this boundary explicitly.
std::string wide_to_utf8(const wchar_t* w);
std::wstring utf8_to_wide(const std::string& s);
std::wstring exe_path_wide();
// UTF-8 directory that holds agent.exe, "." when it cannot be determined.
std::string exe_dir();
// %ProgramData%\MyRemote, the location a service-installed agent can read and
// write even when the exe sits in a protected directory.
std::wstring program_data_dir();

struct AgentPaths {
    std::string config;   // config.json to load
    std::string log_dir;  // where agent.log / service.log / host.status go
};

// --config wins; then %ProgramData%\MyRemote once it exists; then the exe
// directory so a build-tree agent keeps working with a local config.json.
AgentPaths resolve_paths(const std::string& cli_override);

// The session that owns the physical console, even while it is disconnected.
// Returns false when that cannot be proven: callers must then leave the host
// where it is rather than move it somewhere plausible-but-wrong. A session on
// an "RDP-*" station is never accepted.
// "how" receives which tier decided; "table" receives "1 Console(0); ..." for
// the command line, so a remote machine can be diagnosed without a debugger.
bool console_session(DWORD* out, std::string* how = nullptr,
                     std::string* table = nullptr);

// Every session a person could currently be sitting in (console or RDP), for
// the M19 tray proxies: one icon per logged-in session. "active" is the WTS
// connect state; a disconnected RDP session is not active and gets no proxy.
struct SessionInfo {
    DWORD id = 0;
    std::string user;  // empty when nobody is logged into it
    bool active = false;
};
std::vector<SessionInfo> list_sessions();

// Name of the desktop currently receiving keyboard and mouse input:
// "Default", "Winlogon", "SAC-Desktop", ... Empty when it cannot be read.
// Read-only: unlike DesktopFollower this never re-attaches the calling thread.
std::string input_desktop_name();

// Modes the primary display can do, as (width, height) pairs: deduplicated,
// largest area first, capped at 64. Empty when enumeration fails.
std::vector<std::pair<int, int>> list_display_modes();

// Switches the primary display to the given mode for this session only: it is
// applied with plain flags (never CDS_UPDATEREGISTRY), so nothing is written
// and a reboot hands the machine back its own default. The mode is tested
// (CDS_TEST) first. Returns the ChangeDisplaySettingsEx LONG code;
// DISP_CHANGE_SUCCESSFUL == 0.
LONG change_display_mode(int width, int height);

// Runs a console command to completion; returns its exit code (-1 on failure).
int run_command(const std::wstring& command_line);

// A highest-privilege logon task, i.e. one whose agent can drive elevated
// windows. Kept as the explicit fallback for machines where an SCM service is
// not allowed; it cannot do anything before someone logs in, unlike the
// service.
bool set_autostart(bool enable);
extern const wchar_t* const kAutostartTaskName;

// Follows the desktop the keyboard and mouse are really delivered to, which is
// how a host reaches the logon screen (Winlogon) and the UAC consent
// (SAC-Desktop) without ever taking the monitor away from a local user.
// Only one thread per process may own it: SetThreadDesktop is per-thread and
// fails for any thread that has created a window or touched the old desktop's
// GDI objects.
class DesktopFollower {
public:
    DesktopFollower() = default;
    ~DesktopFollower();

    DesktopFollower(const DesktopFollower&) = delete;
    DesktopFollower& operator=(const DesktopFollower&) = delete;

    // Polls the input desktop and re-attaches this thread when its name
    // changes. Returns false when the desktop could not be read or reached.
    bool update(std::string* name, bool* changed);
    const std::string& name() const { return name_; }
    // True while the credential UI or a consent prompt owns the input.
    bool on_secure_desktop() const { return on_secure_desktop_; }

private:
    HDESK held_ = nullptr;  // our own reference to the desktop we are on
    std::string name_ = "Default";
    bool on_secure_desktop_ = false;
    bool logged_denial_ = false;
};

}  // namespace win32util
