// Minimal Prometheus text-exposition registry — no external dependency.
// Direct port of app/internal/obs/metrics.go.
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace sb::obs {

using Labels = std::map<std::string, std::string>;  // sorted by key for a stable series id

class Metrics {
 public:
  Metrics();

  void CounterAdd(const std::string &name, const Labels &labels, double v);
  void GaugeSet(const std::string &name, const Labels &labels, double v);
  void GaugeAdd(const std::string &name, const Labels &labels, double v);
  // Observe records a value into sb_stage_duration_seconds{stage=...}.
  void Observe(const std::string &stage, double seconds);

  std::string Render();

 private:
  struct Histogram {
    std::vector<double> buckets;  // upper bounds, ascending
    std::vector<uint64_t> counts;
    double sum = 0;
    uint64_t count = 0;
  };

  void Declare(const std::string &name, const std::string &type, const std::string &help);

  std::mutex mu_;
  std::map<std::string, double> counters_;
  std::map<std::string, double> gauges_;
  std::map<std::string, Histogram> histograms_;
  std::map<std::string, std::string> help_;
  std::map<std::string, std::string> type_;
};

}  // namespace sb::obs
