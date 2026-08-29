#pragma once

#include <windows.h>

#include <string>

// The MyRemote agent service. It runs in session 0 as LocalSystem and does one
// thing: keep exactly one capture/input host alive inside the session that
// owns the physical console. It never opens a socket, never touches GDI and
// never shows UI - that is what makes "one device row per machine" a structural
// property rather than a lock that could race.
namespace svc {

extern const wchar_t* const kServiceName;
extern const wchar_t* const kDisplayName;

// Entry point for `agent.exe --service`; returns the process exit code.
int run_as_service();

// Registers (or re-points an already registered) service and starts it.
bool install_or_update(std::wstring* why);
bool uninstall(std::wstring* why);
bool start(std::wstring* why);
bool stop(std::wstring* why);

// True when the service is registered, whatever its state.
bool is_installed();

// Human-readable status for `agent.exe --service-state`.
std::string query();

}  // namespace svc
