#pragma once

#include <functional>
#include <mutex>

// Executes the reconnect attempt callback on demand (supervisor loop owns
// the backoff timing).
class AutoReconnect {
public:
    using ReconnectCallback = std::function<bool()>;

    void set_callback(ReconnectCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(callback);
    }

    bool try_once() {
        ReconnectCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = callback_;
        }
        return callback ? callback() : false;
    }

private:
    std::mutex mutex_;
    ReconnectCallback callback_;
};
