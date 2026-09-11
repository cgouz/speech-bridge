#include "log.h"

#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>

namespace sb::obs {

namespace {

Level ParseLevel(const std::string &s) {
  if (s == "debug") return Level::kDebug;
  if (s == "warn") return Level::kWarn;
  if (s == "error") return Level::kError;
  return Level::kInfo;
}

const char *LevelName(Level l) {
  switch (l) {
    case Level::kDebug: return "DEBUG";
    case Level::kWarn: return "WARN";
    case Level::kError: return "ERROR";
    default: return "INFO";
  }
}

std::string NowRFC3339() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto us = duration_cast<microseconds>(now.time_since_epoch()) % 1000000;
  std::time_t t = system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  std::ostringstream os;
  os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
  os << '.' << std::setfill('0') << std::setw(6) << us.count() << 'Z';
  return os.str();
}

// Serializes one line and writes it to stdout under a process-wide lock (log
// lines must not interleave across concurrent writers).
std::mutex &StdoutMutex() {
  static std::mutex m;
  return m;
}

}  // namespace

Logger::Logger(std::string format, std::string level)
    : format_(std::move(format)), level_(ParseLevel(level)) {}

Logger Logger::With(const Fields &fields) const {
  Logger l = *this;
  l.base_fields_.insert(l.base_fields_.end(), fields.begin(), fields.end());
  return l;
}

void Logger::Debug(const std::string &msg, const Fields &fields) const { Log(Level::kDebug, msg, fields); }
void Logger::Info(const std::string &msg, const Fields &fields) const { Log(Level::kInfo, msg, fields); }
void Logger::Warn(const std::string &msg, const Fields &fields) const { Log(Level::kWarn, msg, fields); }
void Logger::Error(const std::string &msg, const Fields &fields) const { Log(Level::kError, msg, fields); }

void Logger::Log(Level lvl, const std::string &msg, const Fields &fields) const {
  if (lvl < level_) return;

  std::string line;
  if (format_ == "text") {
    std::ostringstream os;
    os << "time=" << NowRFC3339() << " level=" << LevelName(lvl) << " msg=" << std::quoted(msg);
    for (auto &[k, v] : base_fields_) os << ' ' << k << '=' << v.dump();
    for (auto &[k, v] : fields) os << ' ' << k << '=' << v.dump();
    line = os.str();
  } else {
    nlohmann::ordered_json j;
    j["time"] = NowRFC3339();
    j["level"] = LevelName(lvl);
    j["msg"] = msg;
    for (auto &[k, v] : base_fields_) j[k] = v;
    for (auto &[k, v] : fields) j[k] = v;
    line = j.dump();
  }

  std::lock_guard<std::mutex> g(StdoutMutex());
  std::cout << line << std::endl;  // endl, not '\n': flush every line (crash-safe, matches slog's behavior)
}

std::string NewID() {
  static std::mutex m;
  static std::mt19937_64 rng{std::random_device{}()};
  std::lock_guard<std::mutex> g(m);
  uint64_t a = rng(), b = rng();
  char buf[25];
  std::snprintf(buf, sizeof(buf), "%016lx%08lx", static_cast<unsigned long>(a),
                static_cast<unsigned long>(b & 0xFFFFFFFFu));
  return std::string(buf, 24);
}

}  // namespace sb::obs
