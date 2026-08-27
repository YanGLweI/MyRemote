#include "heartbeat.hpp"

#include "log.hpp"

HeartbeatKeeper::HeartbeatKeeper() : interval_ms_(3000) {}

HeartbeatKeeper::~HeartbeatKeeper() {
    stop();
}

void HeartbeatKeeper::start(int interval_ms, HeartbeatCallback callback) {
    if (callback_) {
        mlog::warn("Heartbeat already running");
        return;
    }
    
    interval_ms_ = interval_ms;
    callback_ = std::move(callback);
    should_stop_.store(false, std::memory_order_release);
    
    heartbeat_thread_ = std::thread(&HeartbeatKeeper::heartbeat_loop, this);
}

void HeartbeatKeeper::stop() {
    should_stop_.store(true, std::memory_order_release);
    
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    
    callback_ = nullptr;
}

void HeartbeatKeeper::heartbeat_loop() {
    while (!should_stop_.load(std::memory_order_acquire)) {
        // Send heartbeat
        if (callback_) {
            bool success = callback_();
            if (!success) {
                mlog::warn("Heartbeat send failed");
            }
        }
        
        // Wait for next interval
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
    }
}
