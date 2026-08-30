#pragma once

#include <string>

namespace config {

// Client (被控端) configuration, loaded from config.json next to agent.exe.
struct ClientConfig {
    std::string server_ip = "127.0.0.1";
    int server_port = 7500;
    std::string secret_key = "default_secret_key_12345";
    std::string device_name;       // empty = auto-generate from hostname
    std::string control_password;  // secondary verification password (P2)
    // Long edge of the encoded picture; wider desktops are downscaled to it
    // while input mapping keeps using the real desktop size. 0 = native.
    int max_encode_width = 1920;
    // The person at the machine may hide the notification-area icon. It is a
    // setting and not a one-off click: after a reboot the tray has to come back
    // only if they want it to, and the config dialog is the way to say so.
    bool tray_icon = true;

    static ClientConfig load(const std::string& path);
    // Parses the same field set load() reads; empty/absent keys keep defaults.
    // The tray proxy uses it to hand a config received over its pipe to save().
    static ClientConfig from_json(const std::string& text);
    static bool save(const ClientConfig& cfg, const std::string& path);
};

// Server (控制端) configuration, loaded from server_config.json.
struct ServerConfig {
    int listening_port = 7500;
    std::string bind_address = "0.0.0.0";
    int max_connections = 100;
    std::string secret_key = "default_secret_key_12345";
    std::string log_file = "control_server.log";

    static ServerConfig load(const std::string& path);
    // Writes every field, not just the ones a caller edited: a partial write
    // would reset the rest to whatever the defaults happen to be.
    static bool save(const ServerConfig& cfg, const std::string& path);
};

}  // namespace config
