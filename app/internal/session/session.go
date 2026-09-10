// Package session holds the per-connection state machine for a live
// speech-to-speech stream: one speaker, one target language. Transport-agnostic
// — the stream package adapts WebSocket frames to these calls.
//
// Meeting rooms / multi-participant routing are explicitly out of scope.
package session

import (
	"encoding/binary"
	"math"
	"strings"
	"sync"

	"github.com/cgouz/speech-bridge/app/internal/core"
	"github.com/cgouz/speech-bridge/app/internal/pipeline"
)

// Msg is a server->client event. Binary carries the PCM frame that follows an
// "audio" header (nil for every other type).
type Msg struct {
	Type       string  `json:"type"` // partial|transcript|translation|audio|error|done
	Text       string  `json:"text,omitempty"`
	Seq        int     `json:"seq"`
	T0MS       int64   `json:"t0_ms,omitempty"`
	T1MS       int64   `json:"t1_ms,omitempty"`
	NSamples   int     `json:"n_samples,omitempty"`
	SampleRate int     `json:"sample_rate,omitempty"`
	Code       string  `json:"code,omitempty"`
	Stage      string  `json:"stage,omitempty"`

	Binary []float32 `json:"-"`
}

// Start is the client's opening frame.
type Start struct {
	Type       string `json:"type"`
	SourceLang string `json:"source_lang"`
	TargetLang string `json:"target_lang"`
	Voice      string `json:"voice"`
	SampleRate int    `json:"sample_rate"`
	Format     string `json:"format"` // "f32" | "s16"
}

// Session drives one speaker stream.
type Session struct {
	pipe   *pipeline.Pipeline
	send   func(Msg)
	stt    core.STTStream
	cfg    Start

	mu        sync.Mutex
	seq       int             // next sentence index to assign
	pending   sync.WaitGroup  // in-flight sentence workers
	reorder   map[int][]float32
	reorderSR map[int]int
	nextEmit  int
}

// New creates a Session and its STT stream. send must be safe to call from
// multiple goroutines (the stream package serializes writes).
func New(p *pipeline.Pipeline, s Start, send func(Msg)) (*Session, error) {
	if s.SampleRate == 0 {
		s.SampleRate = 16000
	}
	if s.Format == "" {
		s.Format = "f32"
	}
	sess := &Session{
		pipe: p, send: send, cfg: s,
		reorder: map[int][]float32{}, reorderSR: map[int]int{},
	}
	if p.HasSTT() {
		st, err := p.NewSTTStream(s.SourceLang)
		if err != nil {
			return nil, err
		}
		sess.stt = st
	}
	return sess, nil
}

// PushAudio decodes one binary PCM frame and feeds the STT stream.
func (s *Session) PushAudio(frame []byte) {
	if s.stt == nil {
		return
	}
	pcm := decodeFrame(frame, s.cfg.Format)
	if s.cfg.SampleRate != 16000 {
		pcm = pipeline.ResampleLinear(pcm, s.cfg.SampleRate, 16000)
	}
	evs, err := s.stt.Feed(pcm)
	if err != nil {
		s.send(Msg{Type: "error", Code: "stt_error", Stage: "stt", Text: err.Error()})
		return
	}
	s.handleEvents(evs)
}

// Stop flushes the STT tail and waits for in-flight sentences, then emits done.
func (s *Session) Stop() {
	if s.stt != nil {
		if evs, err := s.stt.Finish(); err == nil {
			s.handleEvents(evs)
		}
	}
	s.pending.Wait()
	s.flushReorder()
	s.send(Msg{Type: "done", Seq: s.seq})
}

// Close releases the STT stream (call after Stop).
func (s *Session) Close() {
	if s.stt != nil {
		s.stt.Close()
	}
}

func (s *Session) handleEvents(evs []core.STTEvent) {
	for _, e := range evs {
		switch e.Kind {
		case core.STTPartial:
			s.send(Msg{Type: "partial", Text: e.Text, Seq: s.peekSeq(), T1MS: e.EndMS})
		case core.STTFinalEOU:
			for _, sent := range splitOrWhole(e.Text) {
				s.dispatch(sent, e.StartMS, e.EndMS)
			}
		}
	}
}

// dispatch assigns a seq, emits the transcript, and runs MT->TTS in a worker so
// STT keeps consuming audio (stage overlap).
func (s *Session) dispatch(sentence string, t0, t1 int64) {
	s.mu.Lock()
	seq := s.seq
	s.seq++
	s.mu.Unlock()

	s.send(Msg{Type: "transcript", Text: sentence, Seq: seq, T0MS: t0, T1MS: t1})

	s.pending.Add(1)
	go func() {
		defer s.pending.Done()
		src := s.cfg.SourceLang
		if src == "" {
			src = "auto"
		}

		translated := sentence
		if out, ok, err := s.pipe.Translate(sentence, src, s.cfg.TargetLang); err != nil {
			s.send(Msg{Type: "error", Code: "mt_error", Stage: "mt", Seq: seq, Text: err.Error()})
		} else if ok {
			translated = pipeline.ForDisplay(out, s.cfg.TargetLang)
			s.send(Msg{Type: "translation", Text: translated, Seq: seq})
		} else {
			s.send(Msg{Type: "error", Code: "mt_unavailable", Stage: "mt", Seq: seq})
		}

		pcm, sr, ok, err := s.pipe.Synth(pipeline.ForTTS(translated, s.cfg.TargetLang), s.cfg.TargetLang, s.cfg.Voice)
		if err != nil {
			s.send(Msg{Type: "error", Code: "tts_error", Stage: "tts", Seq: seq, Text: err.Error()})
			s.markReorder(seq, nil, 0)
			return
		}
		if !ok {
			s.send(Msg{Type: "error", Code: "tts_unavailable", Stage: "tts", Seq: seq})
			s.markReorder(seq, nil, 0)
			return
		}
		s.markReorder(seq, pcm, sr)
	}()
}

// markReorder stores a sentence's audio and emits every ready sentence in seq
// order (small reorder buffer).
func (s *Session) markReorder(seq int, pcm []float32, sr int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.reorder[seq] = pcm
	s.reorderSR[seq] = sr
	for {
		pcm, ok := s.reorder[s.nextEmit]
		if !ok {
			return
		}
		sr := s.reorderSR[s.nextEmit]
		delete(s.reorder, s.nextEmit)
		delete(s.reorderSR, s.nextEmit)
		if len(pcm) > 0 {
			s.send(Msg{Type: "audio", Seq: s.nextEmit, NSamples: len(pcm), SampleRate: sr, Binary: pcm})
		}
		s.nextEmit++
	}
}

func (s *Session) flushReorder() {
	s.mu.Lock()
	defer s.mu.Unlock()
	// emit anything left in order, skipping gaps
	keys := make([]int, 0, len(s.reorder))
	for k := range s.reorder {
		keys = append(keys, k)
	}
	for _, k := range sortInts(keys) {
		pcm := s.reorder[k]
		if len(pcm) > 0 {
			s.send(Msg{Type: "audio", Seq: k, NSamples: len(pcm), SampleRate: s.reorderSR[k], Binary: pcm})
		}
		delete(s.reorder, k)
	}
}

func (s *Session) peekSeq() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.seq
}

// ---- helpers --------------------------------------------------------

func splitOrWhole(text string) []string {
	s := pipeline.Sentences(text)
	if len(s) == 0 && strings.TrimSpace(text) != "" {
		return []string{strings.TrimSpace(text)}
	}
	return s
}

func decodeFrame(b []byte, format string) []float32 {
	switch format {
	case "s16":
		n := len(b) / 2
		out := make([]float32, n)
		for i := 0; i < n; i++ {
			out[i] = float32(int16(binary.LittleEndian.Uint16(b[i*2:]))) / 32768.0
		}
		return out
	default: // f32 little-endian
		n := len(b) / 4
		out := make([]float32, n)
		for i := 0; i < n; i++ {
			out[i] = math.Float32frombits(binary.LittleEndian.Uint32(b[i*4:]))
		}
		return out
	}
}

func sortInts(a []int) []int {
	for i := 1; i < len(a); i++ {
		for j := i; j > 0 && a[j-1] > a[j]; j-- {
			a[j-1], a[j] = a[j], a[j-1]
		}
	}
	return a
}
