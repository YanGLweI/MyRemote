#pragma once

#include <string>

namespace mlog {

// Direct subsequent log lines to the given file (append mode).
void init(const std::string& file_path);

void info(const std::string& message);
void warn(const std::string& message);
void error(const std::string& message);

}  // namespace mlog
