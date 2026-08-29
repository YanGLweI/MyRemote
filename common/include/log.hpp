#pragma once

#include <functional>
#include <string>

namespace mlog {

enum class Level { Info, Warn, Error };

// Called for every line, from whichever thread wrote it, with the level plus
// the same text the log file got (without its newline). Kept free of Qt so this
// header stays usable from the agent and the service host.
using Sink = std::function<void(Level, const std::string&)>;

// Direct subsequent log lines to the given file (append mode).
void init(const std::string& file_path);

// One sink at a time, for the process that wants to show its own log live.
// A shutting-down UI has to clear it before the object behind it dies.
void set_sink(Sink sink);

void info(const std::string& message);
void warn(const std::string& message);
void error(const std::string& message);

}  // namespace mlog
