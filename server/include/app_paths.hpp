#pragma once

#include <string>

// Where the control centre keeps its own files. Both the start-up path and the
// settings page have to agree, or a relative name would mean one place at
// launch and another when it is edited.
namespace app {

std::string exe_dir();

// The title doubles as the identity a second launch searches for, so it lives
// here rather than only inside the widget: renaming the window in one place
// cannot then silently disable the single-instance guard.
inline constexpr const wchar_t* kWindowTitle = L"MyRemote 控制中心";

// server_config.json, next to the program.
std::string config_path();

// A bare file name in the config means "next to the program"; a path that
// already says where it lives is taken as given. Empty stays empty: that means
// the operator asked for no log file.
std::string resolve_log_path(const std::string& configured);

}  // namespace app
