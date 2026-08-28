#pragma once

#include <string>

namespace gui {

// Values shown/edited in the configuration dialog.
struct ConfigUi {
    std::string server_ip;
    int server_port = 7500;
    std::string secret_key;
    std::string device_name;
    std::string control_password;
    std::string config_path;  // shown read-only; where Save writes to
    bool saved = false;       // true when the user pressed Save
};

// Runs the native Win32 configuration dialog. Blocks until closed.
// Returns true if the user saved the configuration.
bool run_config_gui(ConfigUi& cfg);

}  // namespace gui
