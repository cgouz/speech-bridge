// Package pipeline runs the per-sentence STT -> MT -> TTS chain in-process
// (no HTTP between stages). The sentence is the unit of work: MT context is
// 512 tokens and TTS caps ~25 s, so transcripts are split before MT and TTS is
// fed one sentence at a time. Batch and streaming share this code.
package pipeline

import (
	"fmt"
	"strings"
	"sync"

	"github.com/cgouz/speech-bridge/app/internal/core"
	"github.com/cgouz/speech-bridge/app/internal/text"
)

// Engines is the set of loaded cores the pipeline drives. STT and MT may be
// nil (degraded); at least one TTS engine may be nil.
type Engines struct {
	STT    core.STT
	MT     core.MT
	Magpie core.TTS
	Vits   core.TTS
}

// Pipeline holds the long-lived, gated inference contexts. One MT context and
// one context per TTS engine, each serialized by a 1-slot gate so a sentence in
// TTS can overlap the next sentence in MT (constraint 4 + stage overlap).
type Pipeline struct {
	eng Engines

	mtMu  sync.Mutex
	mtCtx core.MTCtx

	magpieMu  sync.Mutex
	magpieCtx core.TTSCtx
	vitsMu    sync.Mutex
	vitsCtx   core.TTSCtx

	// language -> engine routing, computed once from what actually loaded.
	route map[string]core.TTSEngine
}

// New builds a Pipeline, creating the shared MT / TTS contexts up front.
func New(eng Engines) (*Pipeline, error) {
	p := &Pipeline{eng: eng, route: map[string]core.TTSEngine{}}

	if eng.MT != nil {
		c, err := eng.MT.NewCtx()
		if err != nil {
			return nil, fmt.Errorf("pipeline: mt ctx: %w", err)
		}
		p.mtCtx = c
	}
	if eng.Magpie != nil {
		c, err := eng.Magpie.NewCtx()
		if err != nil {
			return nil, fmt.Errorf("pipeline: magpie ctx: %w", err)
		}
		p.magpieCtx = c
		for _, l := range eng.Magpie.Languages() {
			p.route[normLang(l)] = core.EngineMagpie
		}
	}
	if eng.Vits != nil {
		c, err := eng.Vits.NewCtx()
		if err != nil {
			return nil, fmt.Errorf("pipeline: vits ctx: %w", err)
		}
		p.vitsCtx = c
		for _, l := range eng.Vits.Languages() {
			p.route[normLang(l)] = core.EngineVITS
		}
	}
	return p, nil
}

// Close releases the shared contexts.
func (p *Pipeline) Close() {
	if p.mtCtx != nil {
		p.mtCtx.Close()
	}
	if p.magpieCtx != nil {
		p.magpieCtx.Close()
	}
	if p.vitsCtx != nil {
		p.vitsCtx.Close()
	}
}

// Capabilities reports, per TTS engine, which languages can actually be
// synthesized right now (loaded model + reported language).
func (p *Pipeline) Capabilities() map[string][]string {
	out := map[string][]string{}
	if p.eng.Magpie != nil {
		out["magpie"] = p.eng.Magpie.Languages()
	}
	if p.eng.Vits != nil {
		out["vits"] = p.eng.Vits.Languages()
	}
	return out
}

// HasSTT / HasMT / TTSForLang expose degraded-mode decisions to callers.
func (p *Pipeline) HasSTT() bool { return p.eng.STT != nil }
func (p *Pipeline) HasMT() bool  { return p.mtCtx != nil }

// NewSTTStream opens a streaming STT session (one per speaker connection).
func (p *Pipeline) NewSTTStream(lang string) (core.STTStream, error) {
	if p.eng.STT == nil {
		return nil, ErrNoSTT
	}
	if lang == "" {
		lang = "auto"
	}
	return p.eng.STT.NewStream(lang)
}

// TTSForLang returns whether some loaded engine covers dst.
func (p *Pipeline) TTSForLang(dst string) bool {
	_, ok := p.route[normLang(dst)]
	return ok
}

// Translate runs one sentence through MT (gated). If MT is not loaded it
// returns the input unchanged with ok=false so callers can run degraded.
func (p *Pipeline) Translate(sentence, src, dst string) (out string, ok bool, err error) {
	if p.mtCtx == nil {
		return sentence, false, nil
	}
	p.mtMu.Lock()
	defer p.mtMu.Unlock()
	out, err = p.mtCtx.Translate(sentence, src, dst)
	if err != nil {
		return "", false, err
	}
	return out, true, nil
}

// Synth runs one sentence through the routed TTS engine (gated). Returns
// ok=false (no error) when no loaded engine covers dst.
func (p *Pipeline) Synth(sentence, dst, voice string) (pcm []float32, sampleRate int, ok bool, err error) {
	eng, found := p.route[normLang(dst)]
	if !found {
		return nil, 0, false, nil
	}
	switch eng {
	case core.EngineMagpie:
		p.magpieMu.Lock()
		defer p.magpieMu.Unlock()
		pcm, sampleRate, err = p.magpieCtx.Speak(sentence, dst, voice)
	case core.EngineVITS:
		p.vitsMu.Lock()
		defer p.vitsMu.Unlock()
		pcm, sampleRate, err = p.vitsCtx.Speak(sentence, dst, voice)
	}
	if err != nil {
		return nil, 0, false, err
	}
	return pcm, sampleRate, true, nil
}

// SplitForDisplay converts a translated sentence to the script used for
// captions: MADLAD emits Uzbek in Cyrillic, but uz is shown in Latin.
func ForDisplay(s, dst string) string {
	if normLang(dst) == "uz" && hasCyrillic(s) {
		return text.CyrillicToLatin(s)
	}
	return s
}

// ForTTS returns the sentence in the script the target voice expects. The MMS
// uz voice is trained on Cyrillic (see docs/models.md), matching MADLAD's
// output, so uz passes through unchanged.
func ForTTS(s, dst string) string { return s }

// Sentences splits a transcript into pipeline units.
func Sentences(transcript string) []string { return text.SplitSentences(transcript) }

func normLang(l string) string {
	l = strings.ToLower(strings.TrimSpace(l))
	switch l {
	case "pt", "pt_br":
		return "pt-BR"
	}
	return l
}

func hasCyrillic(s string) bool {
	for _, r := range s {
		if r >= 0x0400 && r <= 0x04FF {
			return true
		}
	}
	return false
}
