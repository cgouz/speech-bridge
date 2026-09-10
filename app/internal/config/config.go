// Package config loads and validates the SB_* environment. Invalid values make
// the process exit at startup, never at request time.
package config

import (
	"fmt"
	"net"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

// Config is the validated runtime configuration.
type Config struct {
	Bind string

	ModelsDir string
	LibDir    string

	STTModel    string // resolved file path ("" = STT disabled)
	MTModel     string
	MagpieModel string
	VITSDir     string

	MTCtx       int
	MaxBodyMB   int
	MaxAudioSec int
	QueueDepth  int
	StreamsMax  int

	AuthToken string

	LogLevel  string // debug|info|warn|error
	LogFormat string // json|text

	Device string // cpu|metal|auto
}

func getenv(key, def string) string {
	if v, ok := os.LookupEnv(key); ok {
		return v
	}
	return def
}

func getint(key string, def int, errs *[]string) int {
	v, ok := os.LookupEnv(key)
	if !ok || v == "" {
		return def
	}
	n, err := strconv.Atoi(v)
	if err != nil {
		*errs = append(*errs, fmt.Sprintf("%s: %q is not an integer", key, v))
		return def
	}
	return n
}

// Load reads the environment and returns a validated Config, or an error
// listing every problem found.
func Load() (*Config, error) {
	var errs []string
	c := &Config{
		Bind:      getenv("SB_BIND", "127.0.0.1:8080"),
		ModelsDir: getenv("SB_MODELS_DIR", "models"),
		LibDir:    getenv("SB_LIB_DIR", "lib"),

		MTCtx:       getint("SB_MT_CTX", 512, &errs),
		MaxBodyMB:   getint("SB_MAX_BODY_MB", 25, &errs),
		MaxAudioSec: getint("SB_MAX_AUDIO_SEC", 120, &errs),
		QueueDepth:  getint("SB_QUEUE_DEPTH", 8, &errs),
		StreamsMax:  getint("SB_STREAMS_MAX", 4, &errs),

		AuthToken: getenv("SB_AUTH_TOKEN", ""),
		LogLevel:  strings.ToLower(getenv("SB_LOG_LEVEL", "info")),
		LogFormat: strings.ToLower(getenv("SB_LOG_FORMAT", "json")),
		Device:    strings.ToLower(getenv("SB_DEVICE", "auto")),
	}

	// Resolve model paths: explicit SB_* wins; otherwise auto-discover under
	// ModelsDir so `make fetch-models && make run` works out of the box.
	c.STTModel = resolveFile(getenv("SB_STT_MODEL", ""), c.ModelsDir, "stt", "*.gguf")
	c.MTModel = resolveFile(getenv("SB_MT_MODEL", ""), c.ModelsDir, "mt", "*.gguf")
	c.MagpieModel = resolveFile(getenv("SB_TTS_MAGPIE_MODEL", ""), c.ModelsDir, "tts_magpie", "*.gguf")
	if d := getenv("SB_TTS_VITS_DIR", ""); d != "" {
		c.VITSDir = d
	} else if dd := filepath.Join(c.ModelsDir, "tts_vits"); isDir(dd) && hasVITSVoice(dd) {
		c.VITSDir = dd
	}

	if _, _, err := net.SplitHostPort(c.Bind); err != nil {
		errs = append(errs, fmt.Sprintf("SB_BIND: %v", err))
	}
	if c.MTCtx < 64 || c.MTCtx > 4096 {
		errs = append(errs, fmt.Sprintf("SB_MT_CTX: %d out of range [64,4096]", c.MTCtx))
	}
	for _, p := range []struct {
		name string
		v    int
	}{{"SB_MAX_BODY_MB", c.MaxBodyMB}, {"SB_MAX_AUDIO_SEC", c.MaxAudioSec},
		{"SB_QUEUE_DEPTH", c.QueueDepth}, {"SB_STREAMS_MAX", c.StreamsMax}} {
		if p.v <= 0 {
			errs = append(errs, fmt.Sprintf("%s: must be > 0, got %d", p.name, p.v))
		}
	}
	switch c.LogLevel {
	case "debug", "info", "warn", "error":
	default:
		errs = append(errs, fmt.Sprintf("SB_LOG_LEVEL: %q (want debug|info|warn|error)", c.LogLevel))
	}
	switch c.LogFormat {
	case "json", "text":
	default:
		errs = append(errs, fmt.Sprintf("SB_LOG_FORMAT: %q (want json|text)", c.LogFormat))
	}
	switch c.Device {
	case "cpu", "metal", "auto":
	default:
		errs = append(errs, fmt.Sprintf("SB_DEVICE: %q (want cpu|metal|auto)", c.Device))
	}
	if !isDir(c.LibDir) {
		errs = append(errs, fmt.Sprintf("SB_LIB_DIR: %q is not a directory", c.LibDir))
	}

	if len(errs) > 0 {
		sort.Strings(errs)
		return nil, fmt.Errorf("invalid configuration:\n  - %s", strings.Join(errs, "\n  - "))
	}
	return c, nil
}

// resolveFile returns explicit if set (validated to exist), else the first file
// matching pattern under modelsDir/sub, else "".
func resolveFile(explicit, modelsDir, sub, pattern string) string {
	if explicit != "" {
		if fileExists(explicit) {
			return explicit
		}
		return "" // explicit but missing -> disabled; /ready will report it
	}
	matches, _ := filepath.Glob(filepath.Join(modelsDir, sub, pattern))
	sort.Strings(matches)
	for _, m := range matches {
		if fileExists(m) {
			return m
		}
	}
	return ""
}

func hasVITSVoice(dir string) bool {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return false
	}
	for _, e := range entries {
		if e.IsDir() && fileExists(filepath.Join(dir, e.Name(), "model.onnx")) {
			return true
		}
	}
	return false
}

func fileExists(p string) bool {
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}

func isDir(p string) bool {
	st, err := os.Stat(p)
	return err == nil && st.IsDir()
}
