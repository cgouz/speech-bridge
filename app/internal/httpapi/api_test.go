package httpapi

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"io"
	"log/slog"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/cgouz/speech-bridge/app/internal/config"
	"github.com/cgouz/speech-bridge/app/internal/core"
	"github.com/cgouz/speech-bridge/app/internal/obs"
	"github.com/cgouz/speech-bridge/app/internal/pipeline"
)

func testServer(t *testing.T, eng pipeline.Engines, state map[string]string) http.Handler {
	t.Helper()
	p, err := pipeline.New(eng)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(p.Close)
	cfg := &config.Config{
		Bind: "127.0.0.1:0", LibDir: ".", MTCtx: 512, MaxBodyMB: 25, MaxAudioSec: 120,
		QueueDepth: 8, StreamsMax: 4, LogLevel: "error", LogFormat: "json",
	}
	s := New(Deps{
		Config: cfg, Pipeline: p,
		Logger:    slog.New(slog.NewTextHandler(io.Discard, nil)),
		Metrics:   obs.NewMetrics(),
		CoreState: state,
	})
	return s.http.Handler
}

func silentWAV(seconds, rate int) []byte {
	var buf bytes.Buffer
	_ = encodeWAV(&buf, make([]float32, seconds*rate), rate)
	return buf.Bytes()
}

func TestHealthReady(t *testing.T) {
	h := testServer(t, pipeline.Engines{
		STT: &core.FakeSTT{}, MT: &core.FakeMT{}, Vits: &core.FakeTTS{Langs: []string{"uz"}},
	}, map[string]string{"stt": "ok", "mt": "ok", "tts_magpie": "not configured", "tts_vits": "ok"})

	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, httptest.NewRequest("GET", "/health", nil))
	if rr.Code != 200 {
		t.Fatalf("/health = %d", rr.Code)
	}

	rr = httptest.NewRecorder()
	h.ServeHTTP(rr, httptest.NewRequest("GET", "/ready", nil))
	if rr.Code != 200 {
		t.Fatalf("/ready = %d body=%s", rr.Code, rr.Body)
	}
}

func TestReadyFailsWithoutSTT(t *testing.T) {
	h := testServer(t, pipeline.Engines{MT: &core.FakeMT{}}, map[string]string{
		"stt": "core: model load failed", "mt": "ok",
	})
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, httptest.NewRequest("GET", "/ready", nil))
	if rr.Code != http.StatusServiceUnavailable {
		t.Fatalf("/ready = %d, want 503", rr.Code)
	}
}

func TestCapabilities(t *testing.T) {
	h := testServer(t, pipeline.Engines{
		Magpie: &core.FakeTTS{Langs: []string{"en", "de"}},
		Vits:   &core.FakeTTS{Langs: []string{"uz", "ru"}},
	}, map[string]string{"tts_magpie": "ok", "tts_vits": "ok"})

	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, httptest.NewRequest("GET", "/v1/capabilities", nil))
	if rr.Code != 200 {
		t.Fatalf("code %d", rr.Code)
	}
	var body struct {
		TTS map[string][]string `json:"tts"`
	}
	json.Unmarshal(rr.Body.Bytes(), &body)
	if len(body.TTS["magpie"]) != 2 || body.TTS["vits"][0] != "ru" {
		t.Errorf("capabilities tts = %+v", body.TTS)
	}
}

func TestSpeechToSpeechMultipart(t *testing.T) {
	h := testServer(t, pipeline.Engines{
		STT: &core.FakeSTT{Utterances: []string{"Привет мир."}},
		MT:  &core.FakeMT{Fn: func(txt, _, dst string) (string, error) { return "salom", nil }},
		Vits: &core.FakeTTS{Langs: []string{"uz"}, SampleRate: 22050},
	}, map[string]string{"stt": "ok", "mt": "ok", "tts_vits": "ok"})

	var body bytes.Buffer
	mw := multipart.NewWriter(&body)
	mw.WriteField("source_lang", "ru")
	mw.WriteField("target_lang", "uz")
	fw, _ := mw.CreateFormFile("file", "clip.wav")
	fw.Write(silentWAV(1, 16000))
	mw.Close()

	req := httptest.NewRequest("POST", "/v1/speech-to-speech", &body)
	req.Header.Set("Content-Type", mw.FormDataContentType())
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, req)

	if rr.Code != 200 {
		t.Fatalf("code %d body=%s", rr.Code, rr.Body)
	}
	var resp s2sResponse
	json.Unmarshal(rr.Body.Bytes(), &resp)
	if resp.Transcript != "Привет мир." || resp.Translation != "salom" {
		t.Errorf("resp = %+v", resp)
	}
	if resp.Stage != "ok" || resp.AudioFormat != "wav" {
		t.Errorf("stage=%q fmt=%q", resp.Stage, resp.AudioFormat)
	}
	if _, err := base64.StdEncoding.DecodeString(resp.Audio); err != nil {
		t.Errorf("audio not valid base64: %v", err)
	}
}

func TestTranslateEndpoint(t *testing.T) {
	h := testServer(t, pipeline.Engines{
		MT: &core.FakeMT{Fn: func(txt, _, dst string) (string, error) { return "[" + dst + "] " + txt, nil }},
	}, map[string]string{"mt": "ok"})

	rr := httptest.NewRecorder()
	req := httptest.NewRequest("POST", "/v1/translate",
		strings.NewReader(`{"text":"Hello. World.","target_lang":"uz"}`))
	h.ServeHTTP(rr, req)
	if rr.Code != 200 {
		t.Fatalf("code %d body=%s", rr.Code, rr.Body)
	}
	var body struct {
		Translation string `json:"translation"`
	}
	json.Unmarshal(rr.Body.Bytes(), &body)
	if !strings.Contains(body.Translation, "[uz] Hello.") {
		t.Errorf("translation = %q", body.Translation)
	}
}

func TestSpeakUnsupportedLang(t *testing.T) {
	h := testServer(t, pipeline.Engines{
		Vits: &core.FakeTTS{Langs: []string{"uz"}},
	}, map[string]string{"tts_vits": "ok"})
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, httptest.NewRequest("POST", "/v1/speak",
		strings.NewReader(`{"text":"hi","lang":"kaa"}`)))
	if rr.Code != http.StatusServiceUnavailable {
		t.Fatalf("code %d, want 503", rr.Code)
	}
}

func TestRejectNonWAV(t *testing.T) {
	h := testServer(t, pipeline.Engines{STT: &core.FakeSTT{}}, map[string]string{"stt": "ok"})
	rr := httptest.NewRecorder()
	req := httptest.NewRequest("POST", "/v1/speech-to-speech?target_lang=uz",
		bytes.NewReader([]byte("this is not a wav file at all")))
	req.Header.Set("Content-Type", "audio/wav")
	h.ServeHTTP(rr, req)
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("code %d, want 400", rr.Code)
	}
}

func TestMetricsEndpoint(t *testing.T) {
	h := testServer(t, pipeline.Engines{STT: &core.FakeSTT{}}, map[string]string{"stt": "ok"})
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, httptest.NewRequest("GET", "/health", nil))
	rr = httptest.NewRecorder()
	h.ServeHTTP(rr, httptest.NewRequest("GET", "/metrics", nil))
	if rr.Code != 200 || !strings.Contains(rr.Body.String(), "sb_requests_total") {
		t.Fatalf("metrics missing sb_requests_total:\n%s", rr.Body)
	}
	if !strings.Contains(rr.Body.String(), `sb_core_up{core="stt"} 1`) {
		t.Errorf("expected sb_core_up stt = 1")
	}
}
