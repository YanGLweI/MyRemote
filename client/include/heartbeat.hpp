#pragma once

#include <chrono>
#include <functional>
#include <thread>
#include <atomic>

// Heartbeat keeper - maintains connection liveness with server
class HeartbeatKeeper {
public:
    using HeartbeatCallback = std::function<bool()>;  // Callback to send heartbeat
    
    HeartbeatKeeper();
    ~HeartbeatKeeper();
    
    // Start heartbeat at specified interval (ms)
    void start(int interval_ms, HeartbeatCallback callback);
    
    // Stop heartbeat
    void stop();
    
private:
    std::thread heartbeat_thread_;
    std::atomic<bool> should_stop_{false};
    int interval_ms_;
    HeartbeatCallback callback_;
    
    void heartbeat_loop();
};
