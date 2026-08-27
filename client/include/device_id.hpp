#pragma once

#include <string>

namespace device {

// Stable hardware fingerprint: MAC of first physical adapter + computer name,
// hashed to 16 hex chars. Used as the unique device identity on the server.
std::string make_device_id();

// Default device name: computer name.
std::string default_device_name();

}  // namespace device
