#include "auto_reconnect.hpp"
#include <chrono>
#include <iostream>

AutoReconnect::AutoReconnect() {}

AutoReconnect::~AutoReconnect() {
    cancel();
}

void AutoReconnect::set_callback(ReconnectCallback callback) {
    callback_ = std::move(callback);
}

void AutoReconnect::on_connection_lost() {
    if (reconnect_active_.exchange(true, std::memory_order_release)) {
        std::cerr << "Already reconnecting, skipping duplicate request" << std::endl;
        return;
    }
    
    should_stop_.store(false, std::memory_order_release);
    reconnect_thread_ = std::thread(&AutoReconnect::reconnect_loop, this);
}

void AutoReconnect::cancel() {
    should_stop_.store(true, std::memory_order_release);
    
    if (reconnect_active_.load(std::memory_order_acquire)) {
        if (reconnect_thread_.joinable()) {
            reconnect_thread_.join();
        }
        
        reconnect_active_.store(false, std::memory_order_release);
    }
}

void AutoReconnect::reconnect_loop() {
    int retry_attempt = 0;
    auto current_delay = std::chrono::milliseconds(INITIAL_DELAY_MS);
    
    while (!should_stop_.load(std::memory_order_acquire) && 
           reconnect_active_.load(std::memory_order_acquire)) {
        
        retry_attempt++;
        std::cout << "Attempting reconnection (attempt: " << retry_attempt << ")" << std::endl;
        
        // Attempt to reconnect
        if (callback_) {
            if (callback_()) {
                std::cout << "Reconnection successful!" << std::endl;
                break;
            }
        }
        
        if (!should_stop_.load(std::memory_order_acquire)) {
            std::cout << "Retry after " << current_delay.count() << "ms" << std::endl;
            
            // Wait before next attempt with exponential backoff
            Sleep(current_delay.count());
            
            // Increase delay for next retry (up to max)
            current_delay = std::min(
                static_cast<int>(current_delay.count() * DELAY_MULTIPLIER),
                MAX_DELAY_MS
            );
            std::chrono::milliseconds retry_delay(current_delay);
            
            // Don't sleep forever in case of cancellation
            for (int i = 0; i < retry_delay.count() && !should_stop_.load(); i += 1000) {
                Sleep(1000);
            }
        }
    }
    
    reconnect_active_.store(false, std::memory_order_release);
}

std::chrono::milliseconds AutoReconnect::next_delay(int attempt) {
    auto base = INITIAL_DELAY_MS;
    auto delay = base * (1 << attempt);  // Exponential backoff
    return std::min(delay, MAX_DELAY_MS);
}
