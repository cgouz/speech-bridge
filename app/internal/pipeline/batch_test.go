package pipeline

import (
	"strings"
	"testing"

	"github.com/cgouz/speech-bridge/app/internal/core"
)

func newFakePipeline(t *testing.T, e Engines) *Pipeline {
	t.Helper()
	p, err := New(e)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	t.Cleanup(p.Close)
	return p
}

func silence(sec float64, rate int) []float32 {
	return make([]float32, int(sec*float64(rate)))
}

func TestBatchHappyPath(t *testing.T) {
	p := newFakePipeline(t, Engines{
		STT: &core.FakeSTT{Utterances: []string{"Привет мир.", "Как дела?"}, FeedsPerUtterance: 2},
		MT:  &core.FakeMT{Fn: func(text, _, dst string) (string, error) { return "[" + dst + "] " + text, nil }},
		Vits: &core.FakeTTS{Langs: []string{"uz"}, SampleRate: 22050},
	})

	res, err := p.Batch(BatchInput{
		Audio: silence(3, 16000), SampleRate: 16000,
		SourceLang: "ru", TargetLang: "uz", Voice: "",
	})
	if err != nil {
		t.Fatalf("Batch: %v", err)
	}
	if res.Stage != StageOK {
		t.Errorf("stage = %q, want ok", res.Stage)
	}
	if res.Transcript != "Привет мир. Как дела?" {
		t.Errorf("transcript = %q", res.Transcript)
	}
	if !strings.Contains(res.Translation, "[uz]") {
		t.Errorf("translation = %q, want it to contain [uz]", res.Translation)
	}
	if len(res.Audio) == 0 || res.SampleRate != 22050 {
		t.Errorf("audio n=%d sr=%d", len(res.Audio), res.SampleRate)
	}
	if res.Timings.TotalMs < 0 || res.Timings.STTMs < 0 {
		t.Errorf("bad timings %+v", res.Timings)
	}
}

func TestBatchEmptyTranscript(t *testing.T) {
	p := newFakePipeline(t, Engines{
		STT:  &core.FakeSTT{Utterances: nil},
		MT:   &core.FakeMT{},
		Vits: &core.FakeTTS{Langs: []string{"uz"}},
	})
	res, err := p.Batch(BatchInput{Audio: silence(1, 16000), SampleRate: 16000, TargetLang: "uz"})
	if err != nil {
		t.Fatalf("Batch: %v", err)
	}
	if res.Stage != StageEmpty {
		t.Errorf("stage = %q, want %q", res.Stage, StageEmpty)
	}
	if res.Translation != "" || len(res.Audio) != 0 {
		t.Errorf("expected empty result, got translation=%q audio=%d", res.Translation, len(res.Audio))
	}
}

func TestBatchNoSTT(t *testing.T) {
	p := newFakePipeline(t, Engines{MT: &core.FakeMT{}, Vits: &core.FakeTTS{}})
	_, err := p.Batch(BatchInput{Audio: silence(1, 16000), SampleRate: 16000, TargetLang: "uz"})
	if err != ErrNoSTT {
		t.Fatalf("err = %v, want ErrNoSTT", err)
	}
}

func TestBatchMTUnavailable(t *testing.T) {
	p := newFakePipeline(t, Engines{
		STT:  &core.FakeSTT{Utterances: []string{"Привет."}},
		Vits: &core.FakeTTS{Langs: []string{"uz"}},
	})
	res, err := p.Batch(BatchInput{Audio: silence(1, 16000), SampleRate: 16000, TargetLang: "uz"})
	if err != nil {
		t.Fatalf("Batch: %v", err)
	}
	if res.Stage != StageMTUnavailable {
		t.Errorf("stage = %q, want %q", res.Stage, StageMTUnavailable)
	}
	// degraded: caption passes the source text through, audio still synthesized
	if res.Translation != "Привет." {
		t.Errorf("translation = %q, want source passthrough", res.Translation)
	}
	if len(res.Audio) == 0 {
		t.Error("expected audio despite MT being down")
	}
}

func TestBatchTTSUnavailable(t *testing.T) {
	p := newFakePipeline(t, Engines{
		STT: &core.FakeSTT{Utterances: []string{"Привет."}},
		MT:  &core.FakeMT{},
	})
	res, err := p.Batch(BatchInput{Audio: silence(1, 16000), SampleRate: 16000, TargetLang: "uz"})
	if err != nil {
		t.Fatalf("Batch: %v", err)
	}
	if res.Stage != StageTTSUnavailable {
		t.Errorf("stage = %q, want %q", res.Stage, StageTTSUnavailable)
	}
	if len(res.Audio) != 0 {
		t.Error("expected no audio when TTS is down")
	}
	if res.Translation == "" {
		t.Error("expected a caption even when TTS is down")
	}
}

func TestBatchResamples(t *testing.T) {
	p := newFakePipeline(t, Engines{
		STT: &core.FakeSTT{Utterances: []string{"Test."}},
		MT:  &core.FakeMT{}, Magpie: &core.FakeTTS{Langs: []string{"en"}},
	})
	// 48 kHz input must be accepted (resampled to 16 kHz internally).
	res, err := p.Batch(BatchInput{Audio: silence(1, 48000), SampleRate: 48000, TargetLang: "en"})
	if err != nil {
		t.Fatalf("Batch: %v", err)
	}
	if res.Transcript == "" {
		t.Error("expected a transcript from resampled audio")
	}
}

func TestRouting(t *testing.T) {
	p := newFakePipeline(t, Engines{
		Magpie: &core.FakeTTS{Langs: []string{"en", "de"}},
		Vits:   &core.FakeTTS{Langs: []string{"uz", "ru"}},
	})
	if !p.TTSForLang("uz") || !p.TTSForLang("en") {
		t.Error("expected uz+en covered")
	}
	if p.TTSForLang("kaa") {
		t.Error("kaa should be uncovered (no voice loaded)")
	}
}
