#include "Log.h"

#include <AsyncConfig.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

using geulog::Level;
using geulog::SUBSYSTEMS;
using geulog::NUM_SUBSYSTEMS;

// Per-subsystem level, indexed by SUBSYSTEMS[] position. Stored as int
// because std::atomic<int> is unconditionally lock-free and portable.
std::array<std::atomic<int>, NUM_SUBSYSTEMS> g_levels;
// Startup snapshot used by reset().
std::array<int, NUM_SUBSYSTEMS> g_snapshot;

// Async queue + worker.
std::mutex g_queue_mu;
std::condition_variable g_queue_cv;
std::deque<std::string> g_queue;
bool g_stop = false;              // guarded by g_queue_mu
std::thread g_worker;
std::atomic<bool> g_worker_active{false};

int subIndex(const char* sub) {
  for (std::size_t i = 0; i < NUM_SUBSYSTEMS; ++i) {
    if (std::strcmp(SUBSYSTEMS[i], sub) == 0) return static_cast<int>(i);
  }
  return -1;
}

const char* levelName(Level l) {
  switch (l) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info:  return "info";
    case Level::Warn:  return "warn";
    case Level::Error: return "error";
    case Level::Off:   return "off";
  }
  return "?";
}

bool parseLevel(const std::string& s, Level& out) {
  if (s == "trace") { out = Level::Trace; return true; }
  if (s == "debug") { out = Level::Debug; return true; }
  if (s == "info")  { out = Level::Info;  return true; }
  if (s == "warn")  { out = Level::Warn;  return true; }
  if (s == "error") { out = Level::Error; return true; }
  if (s == "off")   { out = Level::Off;   return true; }
  return false;
}

void trim(std::string& s) {
  s.erase(0, s.find_first_not_of(" \t"));
  std::size_t e = s.find_last_not_of(" \t\r\n");
  if (e != std::string::npos) s.erase(e + 1);
}

void workerLoop() {
  std::deque<std::string> local;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(g_queue_mu);
      g_queue_cv.wait(lock, []{ return !g_queue.empty() || g_stop; });
      local.swap(g_queue);
      if (g_stop && local.empty()) return;
    }
    for (auto& line : local) {
      // Short writes are possible on a pipe; we tolerate the rare
      // truncated line rather than introduce a retry loop that could
      // block the worker on a stuck consumer.
      ::write(STDOUT_FILENO, line.data(), line.size());
    }
    local.clear();
  }
}

void startWorker() {
  if (g_worker.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(g_queue_mu);
    g_stop = false;
  }
  g_worker = std::thread(workerLoop);
  g_worker_active.store(true, std::memory_order_release);
}

} // anonymous namespace

namespace geulog {

bool shouldLog(const char* sub, Level lvl) {
  int idx = subIndex(sub);
  if (idx < 0) return false;
  return static_cast<int>(lvl) >=
         g_levels[idx].load(std::memory_order_relaxed);
}

namespace detail {

void enqueue(Level lvl, const char* sub, std::string&& msg) {
  std::string line = "[";
  line += levelName(lvl);
  line += "] [";
  line += sub;
  line += "] ";
  line += msg;
  line += "\n";

  if (!g_worker_active.load(std::memory_order_acquire)) {
    // Worker not running (pre-configure, post-shutdown, or configure
    // aborted before startWorker). Write synchronously so startup
    // errors and --version output still reach the sysop. warn/error
    // go to stderr; info/debug/trace to stdout (matches the
    // pre-facade std::cerr / std::cout split).
    int fd = (lvl >= Level::Warn) ? STDERR_FILENO : STDOUT_FILENO;
    (void)::write(fd, line.data(), line.size());
    return;
  }

  {
    std::lock_guard<std::mutex> lock(g_queue_mu);
    g_queue.push_back(std::move(line));
  }
  g_queue_cv.notify_one();
}

} // namespace detail

bool configure(Async::Config& cfg) {
  Level default_level = Level::Info;
  std::map<std::string, Level> per_sub;

  std::string spec;
  cfg.getValue("GLOBAL", "LOG", spec);
  if (!spec.empty()) {
    std::stringstream ss(spec);
    std::string token;
    while (std::getline(ss, token, ',')) {
      auto eq = token.find('=');
      if (eq == std::string::npos) {
        std::cerr << "*** ERROR: LOG config: missing '=' in '"
                  << token << "'" << std::endl;
        return false;
      }
      std::string key = token.substr(0, eq);
      std::string val = token.substr(eq + 1);
      trim(key);
      trim(val);
      Level lvl;
      if (!parseLevel(val, lvl)) {
        std::cerr << "*** ERROR: LOG config: unknown level '"
                  << val << "'" << std::endl;
        return false;
      }
      if (key == "*") {
        default_level = lvl;
      } else if (subIndex(key.c_str()) < 0) {
        std::cerr << "*** WARNING: LOG config: unknown subsystem '"
                  << key << "' ignored" << std::endl;
      } else {
        per_sub[key] = lvl;
      }
    }
  }

  for (std::size_t i = 0; i < NUM_SUBSYSTEMS; ++i) {
    Level lvl = default_level;
    auto it = per_sub.find(SUBSYSTEMS[i]);
    if (it != per_sub.end()) lvl = it->second;
    g_levels[i].store(static_cast<int>(lvl), std::memory_order_relaxed);
    g_snapshot[i] = static_cast<int>(lvl);
  }

  // TRUNK_DEBUG deprecation warning.
  std::string td;
  if (cfg.getValue("GLOBAL", "TRUNK_DEBUG", td) &&
      !td.empty() && td != "0") {
    std::cerr << "*** WARNING: TRUNK_DEBUG is obsolete; use "
                 "LOG=trunk=debug in [GLOBAL] instead" << std::endl;
  }

  startWorker();
  return true;
}

void shutdown() {
  {
    std::lock_guard<std::mutex> lock(g_queue_mu);
    g_stop = true;
  }
  g_queue_cv.notify_all();
  if (g_worker.joinable()) g_worker.join();
  g_worker_active.store(false, std::memory_order_release);
}

bool setLevel(const std::string& sub, const std::string& lvl_s) {
  Level lvl;
  if (!parseLevel(lvl_s, lvl)) return false;
  if (sub == "*") { setDefaultLevel(lvl_s); return true; }
  int idx = subIndex(sub.c_str());
  if (idx < 0) return false;
  g_levels[idx].store(static_cast<int>(lvl), std::memory_order_relaxed);
  return true;
}

void setDefaultLevel(const std::string& lvl_s) {
  Level lvl;
  if (!parseLevel(lvl_s, lvl)) return;
  for (auto& a : g_levels) {
    a.store(static_cast<int>(lvl), std::memory_order_relaxed);
  }
}

std::string snapshot() {
  std::string out;
  for (std::size_t i = 0; i < NUM_SUBSYSTEMS; ++i) {
    Level lvl = static_cast<Level>(
        g_levels[i].load(std::memory_order_relaxed));
    out += SUBSYSTEMS[i];
    out += "=";
    out += levelName(lvl);
    out += "\n";
  }
  return out;
}

void reset() {
  for (std::size_t i = 0; i < NUM_SUBSYSTEMS; ++i) {
    g_levels[i].store(g_snapshot[i], std::memory_order_relaxed);
  }
}

} // namespace geulog
