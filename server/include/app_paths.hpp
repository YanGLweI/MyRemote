#pragma once

#include <string>

// Where the control centre keeps its own files. Both the start-up path and the
// settings page have to agree, or a relative name would mean one place at
// launch and another when it is edited.
namespace app {

std::string exe_dir();

// server_config.json, next to the program.
std::string config_path();

// A bare file name in the config means "next to the program"; a path that
// already says where it lives is taken as given. Empty stays empty: that means
// the operator asked for no log file.
std::string resolve_log_path(const std::string& configured);

}  // namespace app
