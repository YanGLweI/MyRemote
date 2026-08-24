#pragma once

#include <string>
#include <memory>
#include "tunnel_manager.hpp"

// Remote controller - handles remote desktop connection logic
class RemoteController : public QObject {
    Q_OBJECT
    
public:
    explicit RemoteController(TunnelManager& tunnel_manager, QObject* parent = nullptr)
        : QObject(parent), tunnel_manager_(tunnel_manager) {}
    
    // Start remote control session for specified device
    bool start_remote_control(const std::string& device_id);
    
    // Stop current remote control session
    void stop_remote_control();
    
    // Check if actively controlling a device
    bool is_controlling() const { return currently_controlling_.has_value(); }
    
    // Get current device ID being controlled (if any)
    std::optional<std::string> get_currently_controlled_device() const {
        return currently_controlling_;
    }
    
signals:
    void control_started(const std::string& device_id);
    void control_stopped();
    
private:
    TunnelManager& tunnel_manager_;
    std::optional<std::string> currently_controlling_;
    
    // Handle received frame data from client
    void handle_desktop_frame(const std::vector<uint8_t>& encoded_data);
    
    // Forward mouse/keyboard events to client
    bool send_input_event_to_client(SOCKET socket, 
                                   const std::vector<uint8_t>& event_data);
};
