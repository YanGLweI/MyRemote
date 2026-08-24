#pragma once

#include "connection.hpp"
#include <chrono>
#include <functional>

// Auto-reconnect handler with exponential backoff
class AutoReconnect {
public:
    using ReconnectCallback = std::function<bool()>;  // Callback to reconnect
    
    AutoReconnect();
    ~AutoReconnect();
    
    // Set the reconnect callback (should attempt to connect)
    void set_callback(ReconnectCallback callback);
    
    // Called when connection is lost - starts reconnection process
    void on_connection_lost();
    
    // Cancel reconnection
    void cancel();
    
    // Check if reconnection is active
    bool is_reconnecting() const { return reconnect_active_.load(std::memory_order_acquire); }
    
private:
    ReconnectCallback callback_;
    std::thread reconnect_thread_;
    std::atomic<bool> reconnect_active_{false};
    std::atomic<bool> should_stop_{false};
    
    // Initial delay and maximum retry delay (ms)
    static constexpr int INITIAL_DELAY_MS = 5000;
    static constexpr int MAX_DELAY_MS = 60000;
    static constexpr int DELAY_MULTIPLIER = 2;
    
    void reconnect_loop();
    std::chrono::milliseconds next_delay(int attempt);
};
