#include "remote_controller.hpp"
#include <iostream>

RemoteController::RemoteController(TunnelManager& tunnel_manager, QObject* parent)
    : QObject(parent), tunnel_manager_(tunnel_manager) {}

bool RemoteController::start_remote_control(const std::string& device_id) {
    // Check if we're already controlling a device
    if (is_controlling()) {
        std::cerr << "Already controlling device: " << currently_controlled_.value() << std::endl;
        return false;
    }
    
    // Get client session by device ID
    auto session = tunnel_manager_.get_client_by_device_id(device_id);
    if (!session) {
        std::cerr << "Client not found for device: " << device_id << std::endl;
        return false;
    }
    
    if (!session->active) {
        std::cerr << "Client is not active: " << device_id << std::endl;
        return false;
    }
    
    currently_controlling_ = device_id;
    std::cout << "Started remote control session for: " << device_id << std::endl;
    
    // Emit signal to notify UI
    emit control_started(device_id);
    
    // Send START_CAPTURE command to client
    ControlCommand cmd;
    cmd.set_session_id(hash_device_id(device_id));
    cmd.set_command(ControlCommand_CommandType_START_CAPTURE);
    cmd.set_fps_limit(30);  // Default 30fps
    
    // TODO: Serialize command and send via tunnel manager
    // This would use Protocol Buffers encoding
    
    return true;
}

void RemoteController::stop_remote_control() {
    if (!currently_controlling_.has_value()) {
        return;
    }
    
    std::string device_id = currently_controlled_.value();
    std::cout << "Stopping remote control session for: " << device_id << std::endl;
    
    // Send STOP_CAPTURE command via tunnel manager
    
    currently_controlling_.reset();
    emit control_stopped();
}
}

bool RemoteController::is_controlling() const {
    return currently_controlling_.has_value();
}

std::optional<std::string> RemoteController::get_currently_controlled_device() const {
    return currently_controlling_;
}

uint32_t RemoteController::hash_device_id(const std::string& device_id) {
    // Simple hash function for demo purposes
    uint32_t hash = 5381;
    for (char c : device_id) {
        hash = ((hash << 5) + hash) + static_cast<unsigned char>(c);
    }
    return hash;
}

void RemoteController::handle_desktop_frame(const std::vector<uint8_t>& encoded_data) {
    // Called when receiving an encoded frame from the client
    // In production, this would forward to DisplayRenderer for rendering
    
    if (encoded_data.empty()) {
        return;
    }
    
    // For MVP, we just log the reception
    std::cout << "Received frame data: " << encoded_data.size() << " bytes" << std::endl;
    
    // TODO: Forward to display renderer widget in Qt GUI
    // emit(frame_received_signal)(encoded_data);
}

bool RemoteController::send_input_event_to_client(SOCKET socket, 
                                                  const std::vector<uint8_t>& event_data) {
    // Forward mouse/keyboard events to the controlled client
    bool success = tunnel_manager_.send_to_client(socket, event_data);
    
    if (!success) {
        std::cerr << "Failed to send input event to client: " << socket << std::endl;
    }
    
    return success;
}
