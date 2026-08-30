#pragma once

#include <string>

namespace trayproxy {

// Shared between the host (pipe server) and the proxy (pipe client). The _v1
// suffix is the whole compatibility story: during an upgrade an old proxy
// simply never connects to a new host, gives up and exits, and the new host
// spawns a proxy that speaks its dialect.
inline constexpr const wchar_t kPipeName[] =
    L"\\\\.\\pipe\\MyRemoteAgent_TrayProxy_v1";

// One user-IL tray process per logged-in session (M19). Draws the icon in the
// session it was spawned in and forwards every menu action to the session
// host over kPipeName; when the pipe closes, the host is gone and so is the
// reason for this process to exist.
int run(const std::string& config_override);

}  // namespace trayproxy
