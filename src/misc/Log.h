#ifndef LOG_INCLUDED
#define LOG_INCLUDED

#include <cstddef>
#include <sstream>
#include <string>
#include <utility>

namespace Async { class Config; }

namespace geulog {

enum class Level : int {
  Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5
};

// Seven subsystems, matching the design spec. Strings are what sysops
// type in config and PTY commands; the index is how levels are looked
// up on the hot path.
constexpr const char* SUBSYSTEMS[] = {
  "trunk", "satellite", "twin", "client", "mqtt", "redis", "core"
};
constexpr std::size_t NUM_SUBSYSTEMS =
    sizeof(SUBSYSTEMS) / sizeof(SUBSYSTEMS[0]);

// Init: parse [GLOBAL] LOG=, set per-subsystem levels, spawn the worker
// thread. Call once, before LogWriter::start(). Returns false on config
// parse error (error already written to stderr).
bool configure(Async::Config& cfg);

// Stop the worker thread, drain the queue, join. Call on normal exit
// before LogWriter::stop(). Safe to call multiple times.
void shutdown();

// Set one subsystem's level at runtime. Unknown subsystem or level
// string => false. Passing "*" as the subsystem applies setDefaultLevel.
bool setLevel(const std::string& subsystem, const std::string& level);

// Apply `level` to every subsystem.
void setDefaultLevel(const std::string& level);

// Return "<sub>=<lvl>\n..." for each subsystem.
std::string snapshot();

// Restore the per-subsystem levels that configure() set from .conf.
// Does NOT reread .conf from disk.
void reset();

// Hot-path gate — one atomic load and one integer compare.
bool shouldLog(const char* subsystem, Level level);

namespace detail {
  // Async enqueue. Called only after shouldLog() passed.
  void enqueue(Level level, const char* subsystem, std::string&& message);

  // Recursive stream append — C++11-compatible (avoids fold expressions so
  // Log.h can be #included from call sites built at --std=c++11).
  inline void append_all(std::ostringstream&) {}

  template <typename T, typename... Rest>
  inline void append_all(std::ostringstream& os, T&& first, Rest&&... rest) {
    os << std::forward<T>(first);
    append_all(os, std::forward<Rest>(rest)...);
  }

  template <typename... Args>
  inline std::string concat(Args&&... args) {
    std::ostringstream os;
    append_all(os, std::forward<Args>(args)...);
    return os.str();
  }
}

#define _LOG_DEFINE(fnname, lvlenum) \
  template <typename... Args> \
  inline void fnname(const char* sub, Args&&... args) { \
    if (!shouldLog(sub, Level::lvlenum)) return; \
    detail::enqueue(Level::lvlenum, sub, \
                    detail::concat(std::forward<Args>(args)...)); \
  }

_LOG_DEFINE(trace, Trace)
_LOG_DEFINE(debug, Debug)
_LOG_DEFINE(info,  Info)
_LOG_DEFINE(warn,  Warn)
_LOG_DEFINE(error, Error)

#undef _LOG_DEFINE

} // namespace geulog

#endif
