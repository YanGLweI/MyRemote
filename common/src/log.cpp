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
std::string g_path;
Sink g_sink;

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                  static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &w[0], len);
    return w;
}

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf) + "." +
           std::string(ms.count() < 100 ? (ms.count() < 10 ? "0" : "") : "") +
           std::to_string(ms.count());
}

const char* label(Level level) {
    switch (level) {
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
        default: return "INFO";
    }
}

void write_line(Level level, const std::string& message) {
    const std::string body =
        "[" + timestamp() + "] [" + label(level) + "] " + message;
    const std::string line = body + "\n";
    Sink sink;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file.is_open()) {
            g_file << line;
            g_file.flush();
        }
        sink = g_sink;
    }
    // Outside the lock: the file must not wait on what a UI does with the line,
    // and a sink that itself logs would otherwise deadlock on this same mutex.
    // What it gets is exactly what the file got, so the two cannot disagree.
    if (sink) {
        sink(level, body);
    }
    OutputDebugStringA(line.c_str());
}

void close_locked() {
    if (g_file.is_open()) {
        g_file.close();
    }
    g_path.clear();
}
}  // namespace

bool init(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (file_path.empty()) {
        close_locked();
        return false;  // "no file" is a choice, not a failure
    }
    // The candidate is opened while the current file is still held: a path that
    // cannot be written must not cost us the record we already have.
    std::ofstream candidate(utf8_to_wide(file_path), std::ios::app);
    if (!candidate.is_open()) {
        return false;
    }
    g_file = std::move(candidate);  // lets go of whatever it was writing to
    g_path = file_path;
    return true;
}

std::string path() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_path;
}

void set_sink(Sink sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sink = std::move(sink);
}

void info(const std::string& message) { write_line(Level::Info, message); }
void warn(const std::string& message) { write_line(Level::Warn, message); }
void error(const std::string& message) { write_line(Level::Error, message); }

}  // namespace mlog
