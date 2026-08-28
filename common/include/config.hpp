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

    static ClientConfig load(const std::string& path);
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
};

}  // namespace config
