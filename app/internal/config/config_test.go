package config

import (
	"os"
	"path/filepath"
	"testing"
)

func clearEnv(t *testing.T) {
	t.Helper()
	for _, k := range []string{
		"SB_BIND", "SB_MODELS_DIR", "SB_LIB_DIR", "SB_STT_MODEL", "SB_MT_MODEL",
		"SB_TTS_MAGPIE_MODEL", "SB_TTS_VITS_DIR", "SB_MT_CTX", "SB_MAX_BODY_MB",
		"SB_MAX_AUDIO_SEC", "SB_QUEUE_DEPTH", "SB_STREAMS_MAX", "SB_AUTH_TOKEN",
		"SB_LOG_LEVEL", "SB_LOG_FORMAT", "SB_DEVICE",
	} {
		os.Unsetenv(k)
	}
}

func TestDefaults(t *testing.T) {
	clearEnv(t)
	dir := t.TempDir()
	os.Setenv("SB_LIB_DIR", dir)
	os.Setenv("SB_MODELS_DIR", dir)
	defer clearEnv(t)

	c, err := Load()
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if c.Bind != "127.0.0.1:8080" || c.MTCtx != 512 || c.MaxBodyMB != 25 ||
		c.MaxAudioSec != 120 || c.QueueDepth != 8 || c.StreamsMax != 4 ||
		c.LogFormat != "json" || c.LogLevel != "info" {
		t.Errorf("unexpected defaults: %+v", c)
	}
}

func TestInvalidValuesRejected(t *testing.T) {
	clearEnv(t)
	dir := t.TempDir()
	os.Setenv("SB_LIB_DIR", dir)
	os.Setenv("SB_BIND", "not-a-host-port")
	os.Setenv("SB_MT_CTX", "9")
	os.Setenv("SB_QUEUE_DEPTH", "0")
	os.Setenv("SB_LOG_LEVEL", "loud")
	defer clearEnv(t)

	_, err := Load()
	if err == nil {
		t.Fatal("expected an error for invalid config")
	}
	for _, want := range []string{"SB_BIND", "SB_MT_CTX", "SB_QUEUE_DEPTH", "SB_LOG_LEVEL"} {
		if !contains(err.Error(), want) {
			t.Errorf("error missing %s: %v", want, err)
		}
	}
}

func TestModelAutoDiscovery(t *testing.T) {
	clearEnv(t)
	root := t.TempDir()
	os.Setenv("SB_LIB_DIR", root)
	os.Setenv("SB_MODELS_DIR", root)
	defer clearEnv(t)

	mustDir(t, filepath.Join(root, "stt"))
	mustFile(t, filepath.Join(root, "stt", "nemotron.gguf"))
	mustDir(t, filepath.Join(root, "tts_vits", "vits-mms-rus"))
	mustFile(t, filepath.Join(root, "tts_vits", "vits-mms-rus", "model.onnx"))

	c, err := Load()
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if filepath.Base(c.STTModel) != "nemotron.gguf" {
		t.Errorf("STTModel = %q", c.STTModel)
	}
	if c.MTModel != "" {
		t.Errorf("MTModel should be empty (no file): %q", c.MTModel)
	}
	if c.VITSDir == "" {
		t.Errorf("VITSDir should be discovered")
	}
}

func mustDir(t *testing.T, p string)  { t.Helper(); if err := os.MkdirAll(p, 0o755); err != nil { t.Fatal(err) } }
func mustFile(t *testing.T, p string) { t.Helper(); if err := os.WriteFile(p, []byte("x"), 0o644); err != nil { t.Fatal(err) } }

func contains(s, sub string) bool {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return true
		}
	}
	return false
}
