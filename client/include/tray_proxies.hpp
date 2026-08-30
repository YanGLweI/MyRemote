#pragma once

#include <functional>
#include <string>

namespace trayproxies {

// Host side of M19. Keeps exactly one user-IL tray proxy per active logged-in
// session (console and RDP alike), serves the tray pipe, and turns the
// commands that arrive on it into the same effects the in-process tray used to
// have. Portable (no-service) mode never calls into this module.
struct CommandSink {
    std::function<void()> pause;
    std::function<void()> resume;
    std::function<void()> hide;
    std::function<void()> quit;
    // Receives the whole config as one JSON line; returns whether it persisted.
    std::function<bool(const std::string& json)> save_config;
};

// Returns immediately; the acceptor and supervisor run on their own threads.
// child_config is passed through to every spawned proxy as --config.
void start(CommandSink sink, const std::string& child_config);
void stop();

// Called by the host loop with the current truth; diffed internally and pushed
// to every connected proxy (and to each new proxy on connect). Also records
// whether proxies should exist at all (tray_icon).
void broadcast_state(bool paused, bool registered, bool tray_icon,
                     const std::string& server, const std::string& name);

// "2,4" - the sessions that currently have a live proxy, for host.status.
std::string proxy_sessions();

}  // namespace trayproxies
