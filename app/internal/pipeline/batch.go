package pipeline

import (
	"errors"
	"strings"
	"time"

	"github.com/cgouz/speech-bridge/app/internal/core"
)

// ErrNoSTT is returned by Batch when no STT engine is loaded (maps to HTTP 503).
var ErrNoSTT = errors.New("pipeline: STT engine not available")

// Stage names reported in results (stable, machine-readable — see docs/api.md).
const (
	StageOK             = "ok"
	StageEmpty          = "empty_transcript"
	StageMTUnavailable  = "mt_unavailable"
	StageTTSUnavailable = "tts_unavailable"
)

// BatchInput is one speech-to-speech request.
type BatchInput struct {
	Audio      []float32 // mono float32
	SampleRate int
	SourceLang string // "" or "auto" allowed
	TargetLang string
	Voice      string
}

// Timings are per-stage wall-clock milliseconds.
type Timings struct {
	STTMs   int64 `json:"stt_ms"`
	MTMs    int64 `json:"mt_ms"`
	TTSMs   int64 `json:"tts_ms"`
	TotalMs int64 `json:"total_ms"`
}

// BatchResult is the outcome of Batch.
type BatchResult struct {
	Transcript  string
	Translation string
	Audio       []float32
	SampleRate  int
	Timings     Timings
	Stage       string
}

// Batch runs the whole per-sentence pipeline over a finished audio clip.
// Degraded modes never error: a missing MT or TTS yields a 200 result with the
// corresponding Stage. Only a missing STT engine errors (ErrNoSTT).
func (p *Pipeline) Batch(in BatchInput) (*BatchResult, error) {
	if p.eng.STT == nil {
		return nil, ErrNoSTT
	}
	start := time.Now()
	res := &BatchResult{Stage: StageOK}

	src := in.SourceLang
	if src == "" {
		src = "auto"
	}

	// ---- STT ----
	sttStart := time.Now()
	transcript, err := p.transcribe(in.Audio, in.SampleRate, src)
	res.Timings.STTMs = ms(sttStart)
	if err != nil {
		return nil, err
	}
	res.Transcript = transcript
	if strings.TrimSpace(transcript) == "" {
		res.Stage = StageEmpty
		res.Timings.TotalMs = ms(start)
		return res, nil
	}

	// ---- per-sentence MT -> TTS ----
	sentences := Sentences(transcript)
	var translated []string
	var audio []float32
	sampleRate := 0

	for _, s := range sentences {
		mtStart := time.Now()
		out, ok, terr := p.Translate(s, src, in.TargetLang)
		res.Timings.MTMs += ms(mtStart)
		if terr != nil {
			// Treat a per-sentence MT failure as degraded, not fatal.
			out, ok = s, false
		}
		display := out
		if ok {
			// Only transliterate real MT output (MADLAD Cyrillic uz -> Latin).
			display = ForDisplay(out, in.TargetLang)
		} else {
			res.Stage = worstStage(res.Stage, StageMTUnavailable)
		}
		translated = append(translated, display)

		ttsStart := time.Now()
		pcm, sr, spoke, serr := p.Synth(ForTTS(out, in.TargetLang), in.TargetLang, in.Voice)
		res.Timings.TTSMs += ms(ttsStart)
		if serr != nil || !spoke {
			res.Stage = worstStage(res.Stage, StageTTSUnavailable)
			continue
		}
		if sampleRate == 0 {
			sampleRate = sr
		}
		audio = append(audio, pcm...)
	}

	res.Translation = strings.Join(translated, " ")
	res.Audio = audio
	res.SampleRate = sampleRate
	res.Timings.TotalMs = ms(start)
	return res, nil
}

// transcribe feeds the whole clip through the streaming STT and concatenates
// the finalized utterances.
func (p *Pipeline) transcribe(audio []float32, sampleRate int, srcLang string) (string, error) {
	pcm := audio
	if sampleRate != sttSampleRate {
		pcm = ResampleLinear(audio, sampleRate, sttSampleRate)
	}

	stream, err := p.eng.STT.NewStream(srcLang)
	if err != nil {
		return "", err
	}
	defer stream.Close()

	var parts []string
	collect := func(evs []core.STTEvent) {
		for _, e := range evs {
			if e.Kind == core.STTFinalEOU && strings.TrimSpace(e.Text) != "" {
				parts = append(parts, strings.TrimSpace(e.Text))
			}
		}
	}

	for off := 0; off < len(pcm); off += chunkSamples {
		end := off + chunkSamples
		if end > len(pcm) {
			end = len(pcm)
		}
		evs, ferr := stream.Feed(pcm[off:end])
		if ferr != nil {
			return "", ferr
		}
		collect(evs)
	}
	evs, ferr := stream.Finish()
	if ferr != nil {
		return "", ferr
	}
	collect(evs)

	return strings.Join(parts, " "), nil
}

func ms(since time.Time) int64 { return time.Since(since).Milliseconds() }

// worstStage keeps the "most degraded" of two stage labels.
func worstStage(a, b string) string {
	rank := map[string]int{StageOK: 0, StageMTUnavailable: 1, StageTTSUnavailable: 1, StageEmpty: 2}
	if rank[b] > rank[a] {
		return b
	}
	return a
}
