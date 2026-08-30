#pragma once

#include <functional>
#include <string>

#include "config.hpp"

namespace gui {

enum class SaveMode {
    SaveOnly,      // --config-ui: write the file, no agent involved
    SaveAndRun,    // first double-click: save, then this process runs as agent
    SaveAndApply,  // agent already running: save + hot reload in place
};

// Values shown/edited in the configuration dialog.
struct ConfigUi {
    std::string server_ip;
    int server_port = 7500;
    std::string secret_key;
    std::string device_name;
    std::string control_password;
    // The way back for someone who hid the icon from its own menu: without a
    // checkbox here, hiding it would be a one-way door.
    bool tray_icon = true;
    // Not editable here: the controller's quality preset owns the cap, but a
    // save has to carry it through or it silently resets to the default.
    int max_encode_width = 1920;
    std::string config_path;  // shown read-only; where Save writes to
    SaveMode save_mode = SaveMode::SaveOnly;
    // When set, Save hands the finished config to this instead of writing the
    // file itself: the tray proxy runs user-IL and asks the SYSTEM host to
    // write on its behalf. Empty = write config_path directly, as always.
    std::function<bool(const config::ClientConfig&)> save_via;
    bool saved = false;  // true when the user pressed Save
};

// Runs the native Win32 configuration dialog modally (blocks until closed).
// Callable from any thread that can pump messages; returns true if saved.
bool run_config_gui(ConfigUi& cfg);

// Opens the dialog on a dedicated thread without blocking the caller.
// on_closed receives the final state (saved flag) and runs on that thread.
void show_config_gui_async(ConfigUi cfg,
                           std::function<void(const ConfigUi&)> on_closed);

}  // namespace gui
