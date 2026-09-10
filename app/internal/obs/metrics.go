package obs

import (
	"fmt"
	"sort"
	"strings"
	"sync"
)

// Minimal Prometheus text-exposition registry — no external dependency.
// Supports counters, gauges and fixed-bucket histograms, which is all the
// mission's metric set needs.

// Metrics is the process-wide registry.
type Metrics struct {
	mu         sync.Mutex
	counters   map[string]float64
	gauges     map[string]float64
	histograms map[string]*histogram
	help       map[string]string
	typ        map[string]string
}

type histogram struct {
	buckets []float64 // upper bounds, ascending
	counts  []uint64  // per bucket (cumulative computed at render)
	sum     float64
	count   uint64
}

// NewMetrics returns a registry pre-declaring the Speech Bridge metrics.
func NewMetrics() *Metrics {
	m := &Metrics{
		counters:   map[string]float64{},
		gauges:     map[string]float64{},
		histograms: map[string]*histogram{},
		help:       map[string]string{},
		typ:        map[string]string{},
	}
	m.declare("sb_requests_total", "counter", "Total HTTP requests by endpoint and status.")
	m.declare("sb_stage_duration_seconds", "histogram", "Pipeline stage wall-clock seconds.")
	m.declare("sb_active_streams", "gauge", "Currently open WebSocket streams.")
	m.declare("sb_queue_depth", "gauge", "Queued jobs per core.")
	m.declare("sb_core_up", "gauge", "1 if a core loaded and responds, else 0.")
	m.declare("sb_audio_seconds_total", "counter", "Total seconds of audio processed.")
	return m
}

func (m *Metrics) declare(name, typ, help string) {
	m.typ[name] = typ
	m.help[name] = help
}

func key(name string, labels map[string]string) string {
	if len(labels) == 0 {
		return name
	}
	keys := make([]string, 0, len(labels))
	for k := range labels {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var b strings.Builder
	b.WriteString(name)
	b.WriteByte('{')
	for i, k := range keys {
		if i > 0 {
			b.WriteByte(',')
		}
		fmt.Fprintf(&b, "%s=%q", k, labels[k])
	}
	b.WriteByte('}')
	return b.String()
}

// CounterAdd increments a counter series.
func (m *Metrics) CounterAdd(name string, labels map[string]string, v float64) {
	m.mu.Lock()
	m.counters[key(name, labels)] += v
	m.mu.Unlock()
}

// GaugeSet sets a gauge series.
func (m *Metrics) GaugeSet(name string, labels map[string]string, v float64) {
	m.mu.Lock()
	m.gauges[key(name, labels)] = v
	m.mu.Unlock()
}

// GaugeAdd adjusts a gauge series.
func (m *Metrics) GaugeAdd(name string, labels map[string]string, v float64) {
	m.mu.Lock()
	m.gauges[key(name, labels)] += v
	m.mu.Unlock()
}

var stageBuckets = []float64{0.05, 0.1, 0.25, 0.5, 1, 2, 4, 8, 15, 30}

// Observe records a value into sb_stage_duration_seconds{stage=...}.
func (m *Metrics) Observe(stage string, seconds float64) {
	k := key("sb_stage_duration_seconds", map[string]string{"stage": stage})
	m.mu.Lock()
	h := m.histograms[k]
	if h == nil {
		h = &histogram{buckets: stageBuckets, counts: make([]uint64, len(stageBuckets))}
		m.histograms[k] = h
	}
	h.sum += seconds
	h.count++
	for i, ub := range h.buckets {
		if seconds <= ub {
			h.counts[i]++
		}
	}
	m.mu.Unlock()
}

// Render writes the Prometheus text exposition format.
func (m *Metrics) Render() string {
	m.mu.Lock()
	defer m.mu.Unlock()
	var b strings.Builder

	emitHeader := func(name string) {
		if h := m.help[name]; h != "" {
			fmt.Fprintf(&b, "# HELP %s %s\n", name, h)
		}
		if t := m.typ[name]; t != "" {
			fmt.Fprintf(&b, "# TYPE %s %s\n", name, t)
		}
	}

	seen := map[string]bool{}
	baseName := func(series string) string {
		if i := strings.IndexByte(series, '{'); i >= 0 {
			return series[:i]
		}
		return series
	}

	writeSimple := func(store map[string]float64) {
		keys := make([]string, 0, len(store))
		for k := range store {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		for _, k := range keys {
			if bn := baseName(k); !seen[bn] {
				emitHeader(bn)
				seen[bn] = true
			}
			fmt.Fprintf(&b, "%s %g\n", k, store[k])
		}
	}
	writeSimple(m.counters)
	writeSimple(m.gauges)

	hkeys := make([]string, 0, len(m.histograms))
	for k := range m.histograms {
		hkeys = append(hkeys, k)
	}
	sort.Strings(hkeys)
	for _, k := range hkeys {
		h := m.histograms[k]
		bn := baseName(k)
		if !seen[bn] {
			emitHeader(bn)
			seen[bn] = true
		}
		inner := strings.TrimSuffix(strings.TrimPrefix(k, bn+"{"), "}")
		// counts[i] already holds the count of observations <= buckets[i]
		for i, ub := range h.buckets {
			fmt.Fprintf(&b, "%s_bucket{%s,le=\"%g\"} %d\n", bn, inner, ub, h.counts[i])
		}
		fmt.Fprintf(&b, "%s_bucket{%s,le=\"+Inf\"} %d\n", bn, inner, h.count)
		fmt.Fprintf(&b, "%s_sum{%s} %g\n", bn, inner, h.sum)
		fmt.Fprintf(&b, "%s_count{%s} %d\n", bn, inner, h.count)
	}

	return b.String()
}
