#include "metrics.h"

#include <cstdio>
#include <set>
#include <sstream>

namespace sb::obs {

namespace {

std::string FormatG(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%g", v);
  return buf;
}

std::string SeriesKey(const std::string &name, const Labels &labels) {
  if (labels.empty()) return name;
  std::ostringstream os;
  os << name << '{';
  bool first = true;
  for (auto &[k, v] : labels) {  // std::map iterates sorted by key
    if (!first) os << ',';
    first = false;
    os << k << "=\"" << v << '"';
  }
  os << '}';
  return os.str();
}

std::string BaseName(const std::string &series) {
  auto pos = series.find('{');
  return pos == std::string::npos ? series : series.substr(0, pos);
}

const std::vector<double> &StageBuckets() {
  static const std::vector<double> b = {0.05, 0.1, 0.25, 0.5, 1, 2, 4, 8, 15, 30};
  return b;
}

}  // namespace

Metrics::Metrics() {
  Declare("sb_requests_total", "counter", "Total HTTP requests by endpoint and status.");
  Declare("sb_stage_duration_seconds", "histogram", "Pipeline stage wall-clock seconds.");
  Declare("sb_active_streams", "gauge", "Currently open WebSocket streams.");
  Declare("sb_queue_depth", "gauge", "Queued jobs per core.");
  Declare("sb_core_up", "gauge", "1 if a core loaded and responds, else 0.");
  Declare("sb_audio_seconds_total", "counter", "Total seconds of audio processed.");
}

void Metrics::Declare(const std::string &name, const std::string &type, const std::string &help) {
  type_[name] = type;
  help_[name] = help;
}

void Metrics::CounterAdd(const std::string &name, const Labels &labels, double v) {
  std::lock_guard<std::mutex> g(mu_);
  counters_[SeriesKey(name, labels)] += v;
}

void Metrics::GaugeSet(const std::string &name, const Labels &labels, double v) {
  std::lock_guard<std::mutex> g(mu_);
  gauges_[SeriesKey(name, labels)] = v;
}

void Metrics::GaugeAdd(const std::string &name, const Labels &labels, double v) {
  std::lock_guard<std::mutex> g(mu_);
  gauges_[SeriesKey(name, labels)] += v;
}

void Metrics::Observe(const std::string &stage, double seconds) {
  std::string k = SeriesKey("sb_stage_duration_seconds", Labels{{"stage", stage}});
  std::lock_guard<std::mutex> g(mu_);
  auto &h = histograms_[k];
  if (h.buckets.empty()) {
    h.buckets = StageBuckets();
    h.counts.assign(h.buckets.size(), 0);
  }
  h.sum += seconds;
  h.count++;
  for (size_t i = 0; i < h.buckets.size(); i++) {
    if (seconds <= h.buckets[i]) h.counts[i]++;
  }
}

std::string Metrics::Render() {
  std::lock_guard<std::mutex> g(mu_);
  std::ostringstream b;
  std::set<std::string> seen;

  auto emitHeader = [&](const std::string &name) {
    auto hit = help_.find(name);
    if (hit != help_.end() && !hit->second.empty()) b << "# HELP " << name << ' ' << hit->second << '\n';
    auto tit = type_.find(name);
    if (tit != type_.end() && !tit->second.empty()) b << "# TYPE " << name << ' ' << tit->second << '\n';
  };

  auto writeSimple = [&](const std::map<std::string, double> &store) {
    for (auto &[k, v] : store) {  // sorted by key already
      std::string bn = BaseName(k);
      if (seen.insert(bn).second) emitHeader(bn);
      b << k << ' ' << FormatG(v) << '\n';
    }
  };
  writeSimple(counters_);
  writeSimple(gauges_);

  for (auto &[k, h] : histograms_) {
    std::string bn = BaseName(k);
    if (seen.insert(bn).second) emitHeader(bn);
    std::string inner;
    auto open = k.find('{');
    if (open != std::string::npos) inner = k.substr(open + 1, k.size() - open - 2);
    for (size_t i = 0; i < h.buckets.size(); i++) {
      b << bn << "_bucket{" << inner << ",le=\"" << FormatG(h.buckets[i]) << "\"} " << h.counts[i] << '\n';
    }
    b << bn << "_bucket{" << inner << ",le=\"+Inf\"} " << h.count << '\n';
    b << bn << "_sum{" << inner << "} " << FormatG(h.sum) << '\n';
    b << bn << "_count{" << inner << "} " << h.count << '\n';
  }

  return b.str();
}

}  // namespace sb::obs
