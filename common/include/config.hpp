// Configuration module for client and server
#include <string>
#include <fstream>
#include <nlohmann/json.hpp>

namespace config {
    
    struct ClientConfig {
        std::string server_ip = "192.168.1.100";
        int server_port = 7500;
        std::string secret_key = "default_secret_key_12345";
        std::string device_name = "";  // Auto-generated if empty
        std::string quality_mode = "balanced";  // low_latency | balanced | high_quality
        
        // Quality presets
        int fps = 30;
        int bitrate_kbps = 2048;
        
        void apply_preset() {
            if (quality_mode == "low_latency") {
                fps = 30;
                bitrate_kbps = 1500;
            } else if (quality_mode == "high_quality") {
                fps = 60;
                bitrate_kbps = 6000;
            }
            // balanced is default
        }
    };
    
    struct ServerConfig {
        int listening_port = 7500;
        std::string bind_address = "0.0.0.0";
        int max_connections = 100;
        bool enable_logging = true;
        std::string log_file = "control_server.log";
        
        std::array<uint8_t, 16> get_global_encryption_key() const {
            // Generate deterministic key from string (for demo purposes)
            std::array<uint8_t, 16> key{};
            auto hash = std::hash<std::string>{}(secret_key);
            memcpy(key.data(), &hash, sizeof(hash));
            return key;
        }
    };
    
    // Load configuration from JSON file
    static ClientConfig load_client_config(const std::string& path = "config.json") {
        std::ifstream file(path);
        if (!file.good()) {
            std::cout << "Using default client configuration" << std::endl;
            return ClientConfig{};
        }
        
        try {
            nlohmann::json json_data;
            file >> json_data;
            
            ClientConfig config;
            config.server_ip = json_data.value("server_ip", "192.168.1.100");
            config.server_port = json_data.value("server_port", 7500);
            config.secret_key = json_data.value("secret_key", "default_secret_key_12345");
            config.device_name = json_data.value("device_name", "");
            config.quality_mode = json_data.value("quality_mode", "balanced");
            config.apply_preset();
            
            return config;
        } catch (const nlohmann::detail::parse_error& e) {
            std::cerr << "Failed to parse config file: " << e.what() << std::endl;
        }
        
        return ClientConfig{};
    }
    
    static ServerConfig load_server_config(const std::string& path = "server_config.json") {
        std::ifstream file(path);
        if (!file.good()) {
            std::cout << "Using default server configuration" << std::endl;
            return ServerConfig{};
        }
        
        try {
            nlohmann::json json_data;
            file >> json_data;
            
            ServerConfig config;
            config.listening_port = json_data.value("listening_port", 7500);
            config.bind_address = json_data.value("bind_address", "0.0.0.0");
            config.max_connections = json_data.value("max_connections", 100);
            config.enable_logging = json_data.value("enable_logging", true);
            config.log_file = json_data.value("log_file", "control_server.log");
            
            return config;
        } catch (const nlohmann::detail::parse_error& e) {
            std::cerr << "Failed to parse server config: " << e.what() << std::endl;
        }
        
        return ServerConfig{};
    }
    
} // namespace config
