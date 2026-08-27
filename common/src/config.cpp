#include "config.hpp"

#include <fstream>
#include <sstream>

namespace config {

namespace {

// Minimal JSON reader for flat objects: {"key": "string", "num": 123}
// Comments and nesting are unsupported; unknown keys are ignored.
class MiniJson {
public:
    explicit MiniJson(const std::string& text) : text_(text) {}

    bool get_string(const std::string& key, std::string& out) const {
        size_t value_pos = 0;
        if (!find_value(key, value_pos)) {
            return false;
        }
        if (value_pos >= text_.size() || text_[value_pos] != '"') {
            return false;
        }
        size_t end = value_pos + 1;
        std::string value;
        while (end < text_.size()) {
            char c = text_[end];
            if (c == '\\' && end + 1 < text_.size()) {
                value.push_back(text_[end + 1]);
                end += 2;
                continue;
            }
            if (c == '"') {
                out = value;
                return true;
            }
            value.push_back(c);
            ++end;
        }
        return false;
    }

    bool get_int(const std::string& key, int& out) const {
        size_t value_pos = 0;
        if (!find_value(key, value_pos)) {
            return false;
        }
        size_t end = value_pos;
        while (end < text_.size() &&
               (isdigit(static_cast<unsigned char>(text_[end])) || text_[end] == '-')) {
            ++end;
        }
        if (end == value_pos) {
            return false;
        }
        try {
            out = std::stoi(text_.substr(value_pos, end - value_pos));
            return true;
        } catch (...) {
            return false;
        }
    }

private:
    bool find_value(const std::string& key, size_t& value_pos) const {
        std::string needle = "\"" + key + "\"";
        size_t key_pos = text_.find(needle);
        if (key_pos == std::string::npos) {
            return false;
        }
        size_t colon = text_.find(':', key_pos + needle.size());
        if (colon == std::string::npos) {
            return false;
        }
        value_pos = colon + 1;
        while (value_pos < text_.size() &&
               isspace(static_cast<unsigned char>(text_[value_pos]))) {
            ++value_pos;
        }
        return value_pos < text_.size();
    }

    std::string text_;
};

std::string read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.good()) {
        return {};
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

}  // namespace

ClientConfig ClientConfig::load(const std::string& path) {
    ClientConfig cfg;
    std::string text = read_file(path);
    if (text.empty()) {
        return cfg;
    }
    MiniJson json(text);
    json.get_string("server_ip", cfg.server_ip);
    json.get_int("server_port", cfg.server_port);
    json.get_string("secret_key", cfg.secret_key);
    json.get_string("device_name", cfg.device_name);
    json.get_string("control_password", cfg.control_password);
    return cfg;
}

ServerConfig ServerConfig::load(const std::string& path) {
    ServerConfig cfg;
    std::string text = read_file(path);
    if (text.empty()) {
        return cfg;
    }
    MiniJson json(text);
    json.get_int("listening_port", cfg.listening_port);
    json.get_string("bind_address", cfg.bind_address);
    json.get_int("max_connections", cfg.max_connections);
    json.get_string("secret_key", cfg.secret_key);
    json.get_string("log_file", cfg.log_file);
    return cfg;
}

}  // namespace config
