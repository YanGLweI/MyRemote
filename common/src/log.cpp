#include "log.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

namespace mlog {

namespace {
std::mutex g_mutex;
std::ofstream g_file;

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return buf;
}

void write_line(const char* level, const std::string& message) {
    std::string line =
        "[" + timestamp() + "] [" + level + "] " + message + "\n";
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file.is_open()) {
            g_file << line;
            g_file.flush();
        }
    }
    OutputDebugStringA(line.c_str());
}
}  // namespace

void init(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_file.open(file_path, std::ios::app);
}

void info(const std::string& message) { write_line("INFO", message); }
void warn(const std::string& message) { write_line("WARN", message); }
void error(const std::string& message) { write_line("ERROR", message); }

}  // namespace mlog
