package httpapi

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"sort"
	"strings"
	"time"

	"github.com/cgouz/speech-bridge/app/internal/obs"
	"github.com/cgouz/speech-bridge/app/internal/pipeline"
)

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

// /ready — 200 only if every core that is *configured* actually loaded.
func (s *Server) handleReady(w http.ResponseWriter, r *http.Request) {
	ready := true
	cores := map[string]string{}
	for name, st := range s.deps.CoreState {
		cores[name] = st
		if st != "ok" && st != "not configured" {
			ready = false
		}
	}
	// STT unavailable is a hard fail for readiness (speech-to-speech needs it).
	if s.deps.CoreState["stt"] != "ok" {
		ready = false
	}
	code := http.StatusOK
	if !ready {
		code = http.StatusServiceUnavailable
	}
	writeJSON(w, code, map[string]any{"ready": ready, "cores": cores})
}

func (s *Server) handleMetrics(w http.ResponseWriter, r *http.Request) {
	// keep sb_core_up fresh
	for _, name := range []string{"stt", "mt", "tts_magpie", "tts_vits"} {
		up := 0.0
		if s.deps.CoreState[name] == "ok" {
			up = 1
		}
		s.deps.Metrics.GaugeSet("sb_core_up", map[string]string{"core": name}, up)
	}
	w.Header().Set("Content-Type", "text/plain; version=0.0.4")
	_, _ = io.WriteString(w, s.deps.Metrics.Render())
}

func (s *Server) handleCapabilities(w http.ResponseWriter, r *http.Request) {
	caps := s.deps.Pipeline.Capabilities()
	// stable ordering
	for k := range caps {
		sort.Strings(caps[k])
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"cores":     s.deps.CoreState,
		"tts":       caps,
		"stt_langs": []string{"uz", "ru", "kaa", "auto"},
	})
}

// ---- batch endpoints -------------------------------------------------

type s2sResponse struct {
	RequestID   string            `json:"request_id"`
	Transcript  string            `json:"transcript"`
	Translation string            `json:"translation"`
	Audio       string            `json:"audio,omitempty"` // base64 WAV
	AudioFormat string            `json:"audio_format,omitempty"`
	SampleRate  int               `json:"sample_rate,omitempty"`
	Timings     pipeline.Timings  `json:"timings"`
	Stage       string            `json:"stage"`
}

func (s *Server) handleSpeechToSpeech(w http.ResponseWriter, r *http.Request) {
	rid := ridOf(r)
	log := obs.From(r.Context(), s.deps.Logger)

	audio, srcLang, dstLang, voice, err := s.readAudioRequest(r)
	if err != nil {
		s.audioReqError(w, err, rid)
		return
	}
	if dstLang == "" {
		writeError(w, http.StatusBadRequest, "bad_request", "target_lang is required", "", rid)
		return
	}

	res, err := s.deps.Pipeline.Batch(pipeline.BatchInput{
		Audio: audio.Samples, SampleRate: audio.SampleRate,
		SourceLang: srcLang, TargetLang: dstLang, Voice: voice,
	})
	if errors.Is(err, pipeline.ErrNoSTT) {
		writeError(w, http.StatusServiceUnavailable, "stt_unavailable", "speech-to-text engine not loaded", "stt", rid)
		return
	}
	if err != nil {
		log.Error("speech-to-speech failed", "duration_ms", 0, "err", err)
		writeError(w, http.StatusInternalServerError, "pipeline_error", err.Error(), "", rid)
		return
	}
	s.observe(res.Timings)
	s.deps.Metrics.CounterAdd("sb_audio_seconds_total", nil, audio.DurationS)

	log.Info("speech-to-speech",
		"stage", res.Stage, "duration_ms", res.Timings.TotalMs,
		"stt_ms", res.Timings.STTMs, "mt_ms", res.Timings.MTMs, "tts_ms", res.Timings.TTSMs,
		"n_chars_transcript", len(res.Transcript))
	log.Debug("speech-to-speech text", "transcript", res.Transcript, "translation", res.Translation)

	out := s2sResponse{
		RequestID: rid, Transcript: res.Transcript, Translation: res.Translation,
		Timings: res.Timings, Stage: res.Stage,
	}
	if len(res.Audio) > 0 {
		var buf bytes.Buffer
		_ = encodeWAV(&buf, res.Audio, res.SampleRate)
		out.Audio = base64.StdEncoding.EncodeToString(buf.Bytes())
		out.AudioFormat = "wav"
		out.SampleRate = res.SampleRate
	}
	writeJSON(w, http.StatusOK, out)
}

func (s *Server) handleTranscribe(w http.ResponseWriter, r *http.Request) {
	rid := ridOf(r)
	audio, srcLang, _, _, err := s.readAudioRequest(r)
	if err != nil {
		s.audioReqError(w, err, rid)
		return
	}
	if !s.deps.Pipeline.HasSTT() {
		writeError(w, http.StatusServiceUnavailable, "stt_unavailable", "speech-to-text engine not loaded", "stt", rid)
		return
	}
	res, err := s.deps.Pipeline.Batch(pipeline.BatchInput{
		Audio: audio.Samples, SampleRate: audio.SampleRate,
		SourceLang: srcLang, TargetLang: "en", // dst unused; TTS/ MT skipped below
	})
	// We only want the transcript; ignore translation/audio.
	if err != nil {
		writeError(w, http.StatusInternalServerError, "pipeline_error", err.Error(), "", rid)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"request_id": rid, "transcript": res.Transcript, "timings": res.Timings,
	})
}

type translateRequest struct {
	Text       string `json:"text"`
	SourceLang string `json:"source_lang"`
	TargetLang string `json:"target_lang"`
}

func (s *Server) handleTranslate(w http.ResponseWriter, r *http.Request) {
	rid := ridOf(r)
	var req translateRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", "invalid JSON body", "", rid)
		return
	}
	if req.Text == "" || req.TargetLang == "" {
		writeError(w, http.StatusBadRequest, "bad_request", "text and target_lang are required", "", rid)
		return
	}
	if !s.deps.Pipeline.HasMT() {
		writeError(w, http.StatusServiceUnavailable, "mt_unavailable", "translation engine not loaded", "mt", rid)
		return
	}
	var parts []string
	start := time.Now()
	for _, sent := range pipeline.Sentences(req.Text) {
		out, _, err := s.deps.Pipeline.Translate(sent, orAuto(req.SourceLang), req.TargetLang)
		if err != nil {
			writeError(w, http.StatusInternalServerError, "mt_error", err.Error(), "mt", rid)
			return
		}
		parts = append(parts, pipeline.ForDisplay(out, req.TargetLang))
	}
	s.deps.Metrics.Observe("mt", time.Since(start).Seconds())
	writeJSON(w, http.StatusOK, map[string]any{
		"request_id": rid, "translation": strings.Join(parts, " "),
	})
}

type speakRequest struct {
	Text     string `json:"text"`
	Lang     string `json:"lang"`
	Voice    string `json:"voice"`
	Encoding string `json:"encoding"` // "wav" (default) | "base64"
}

func (s *Server) handleSpeak(w http.ResponseWriter, r *http.Request) {
	rid := ridOf(r)
	var req speakRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", "invalid JSON body", "", rid)
		return
	}
	if req.Text == "" || req.Lang == "" {
		writeError(w, http.StatusBadRequest, "bad_request", "text and lang are required", "", rid)
		return
	}
	if !s.deps.Pipeline.TTSForLang(req.Lang) {
		writeError(w, http.StatusServiceUnavailable, "tts_unavailable",
			"no TTS engine loaded for language "+req.Lang, "tts", rid)
		return
	}
	var all []float32
	sr := 0
	start := time.Now()
	for _, sent := range pipeline.Sentences(req.Text) {
		pcm, rate, ok, err := s.deps.Pipeline.Synth(sent, req.Lang, req.Voice)
		if err != nil {
			writeError(w, http.StatusInternalServerError, "tts_error", err.Error(), "tts", rid)
			return
		}
		if !ok {
			writeError(w, http.StatusServiceUnavailable, "tts_unavailable", "no engine for "+req.Lang, "tts", rid)
			return
		}
		if sr == 0 {
			sr = rate
		}
		all = append(all, pcm...)
	}
	s.deps.Metrics.Observe("tts", time.Since(start).Seconds())

	var buf bytes.Buffer
	_ = encodeWAV(&buf, all, sr)
	if req.Encoding == "base64" {
		writeJSON(w, http.StatusOK, map[string]any{
			"request_id": rid, "audio": base64.StdEncoding.EncodeToString(buf.Bytes()),
			"audio_format": "wav", "sample_rate": sr,
		})
		return
	}
	w.Header().Set("Content-Type", "audio/wav")
	w.Header().Set("X-Sample-Rate", itoa(sr))
	_, _ = w.Write(buf.Bytes())
}

// ---- request parsing ------------------------------------------------

// readAudioRequest accepts multipart/form-data (file=<wav>, source_lang,
// target_lang, voice) or a raw audio/wav body with lang query params.
func (s *Server) readAudioRequest(r *http.Request) (a *wavAudio, src, dst, voice string, err error) {
	ct := r.Header.Get("Content-Type")
	var raw []byte

	if strings.HasPrefix(ct, "multipart/form-data") {
		if err = r.ParseMultipartForm(int64(s.deps.Config.MaxBodyMB) << 20); err != nil {
			return nil, "", "", "", errBadMultipart
		}
		src = r.FormValue("source_lang")
		dst = r.FormValue("target_lang")
		voice = r.FormValue("voice")
		f, _, ferr := r.FormFile("file")
		if ferr != nil {
			f, _, ferr = r.FormFile("audio")
		}
		if ferr != nil {
			return nil, "", "", "", errNoFile
		}
		defer f.Close()
		raw, err = io.ReadAll(f)
	} else {
		src = r.URL.Query().Get("source_lang")
		dst = r.URL.Query().Get("target_lang")
		voice = r.URL.Query().Get("voice")
		raw, err = io.ReadAll(r.Body)
	}
	if err != nil {
		return nil, "", "", "", err
	}

	a, derr := decodeWAV(raw)
	if derr != nil {
		return nil, "", "", "", derr
	}
	if a.DurationS > float64(s.deps.Config.MaxAudioSec) {
		return nil, "", "", "", errAudioTooLong
	}
	return a, src, dst, voice, nil
}

var (
	errBadMultipart = errors.New("bad multipart body")
	errNoFile       = errors.New("no audio file in request")
	errAudioTooLong = errors.New("audio exceeds SB_MAX_AUDIO_SEC")
)

func (s *Server) audioReqError(w http.ResponseWriter, err error, rid string) {
	switch {
	case errors.Is(err, errAudioTooLong):
		writeError(w, http.StatusRequestEntityTooLarge, "audio_too_long", err.Error(), "", rid)
	case errors.Is(err, errNotWAV), errors.Is(err, errNoFile), errors.Is(err, errBadMultipart):
		writeError(w, http.StatusBadRequest, "bad_audio", err.Error(), "", rid)
	default:
		var mbErr *http.MaxBytesError
		if errors.As(err, &mbErr) {
			writeError(w, http.StatusRequestEntityTooLarge, "body_too_large", "request body exceeds SB_MAX_BODY_MB", "", rid)
			return
		}
		writeError(w, http.StatusBadRequest, "bad_request", err.Error(), "", rid)
	}
}

func (s *Server) observe(t pipeline.Timings) {
	s.deps.Metrics.Observe("stt", float64(t.STTMs)/1000)
	s.deps.Metrics.Observe("mt", float64(t.MTMs)/1000)
	s.deps.Metrics.Observe("tts", float64(t.TTSMs)/1000)
	s.deps.Metrics.Observe("total", float64(t.TotalMs)/1000)
}

func orAuto(s string) string {
	if s == "" {
		return "auto"
	}
	return s
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	var b [20]byte
	i := len(b)
	for n > 0 {
		i--
		b[i] = byte('0' + n%10)
		n /= 10
	}
	if neg {
		i--
		b[i] = '-'
	}
	return string(b[i:])
}
