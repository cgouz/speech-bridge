package session

import (
	"sync"
	"testing"

	"github.com/cgouz/speech-bridge/app/internal/core"
	"github.com/cgouz/speech-bridge/app/internal/pipeline"
)

type recorder struct {
	mu   sync.Mutex
	msgs []Msg
}

func (r *recorder) send(m Msg) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.msgs = append(r.msgs, m)
}
func (r *recorder) byType(t string) []Msg {
	r.mu.Lock()
	defer r.mu.Unlock()
	var out []Msg
	for _, m := range r.msgs {
		if m.Type == t {
			out = append(out, m)
		}
	}
	return out
}

func TestStreamingProducesOrderedEvents(t *testing.T) {
	p, err := pipeline.New(pipeline.Engines{
		STT: &core.FakeSTT{
			Utterances:        []string{"Привет мир.", "Как дела?", "Хорошо."},
			FeedsPerUtterance: 2,
		},
		MT:   &core.FakeMT{Fn: func(txt, _, dst string) (string, error) { return "T:" + txt, nil }},
		Vits: &core.FakeTTS{Langs: []string{"uz"}, SampleRate: 22050, SamplesPerRune: 50},
	})
	if err != nil {
		t.Fatal(err)
	}
	defer p.Close()

	rec := &recorder{}
	s, err := New(p, Start{Type: "start", SourceLang: "ru", TargetLang: "uz", SampleRate: 16000, Format: "f32"}, rec.send)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	// 3 utterances * 2 feeds = 6 frames of ~250 ms of silence
	frame := make([]byte, 4000*4) // 4000 float32 samples
	for i := 0; i < 6; i++ {
		s.PushAudio(frame)
	}
	s.Stop()

	transcripts := rec.byType("transcript")
	if len(transcripts) != 3 {
		t.Fatalf("got %d transcripts, want 3: %+v", len(transcripts), transcripts)
	}
	for i, m := range transcripts {
		if m.Seq != i {
			t.Errorf("transcript %d has seq %d", i, m.Seq)
		}
	}

	audio := rec.byType("audio")
	if len(audio) != 3 {
		t.Fatalf("got %d audio frames, want 3", len(audio))
	}
	// audio must be emitted in seq order
	for i, m := range audio {
		if m.Seq != i {
			t.Errorf("audio frame %d has seq %d (out of order)", i, m.Seq)
		}
		if len(m.Binary) == 0 || m.SampleRate != 22050 {
			t.Errorf("audio %d: n=%d sr=%d", i, len(m.Binary), m.SampleRate)
		}
	}

	if len(rec.byType("translation")) != 3 {
		t.Errorf("want 3 translations, got %d", len(rec.byType("translation")))
	}
	if len(rec.byType("done")) != 1 {
		t.Errorf("want 1 done")
	}
}

func TestStreamingDegradedNoMT(t *testing.T) {
	p, _ := pipeline.New(pipeline.Engines{
		STT:  &core.FakeSTT{Utterances: []string{"Привет."}},
		Vits: &core.FakeTTS{Langs: []string{"uz"}},
	})
	defer p.Close()

	rec := &recorder{}
	s, _ := New(p, Start{Type: "start", TargetLang: "uz", SampleRate: 16000}, rec.send)
	defer s.Close()
	s.PushAudio(make([]byte, 4000*4))
	s.Stop()

	if len(rec.byType("transcript")) != 1 {
		t.Errorf("want a transcript even without MT")
	}
	errs := rec.byType("error")
	if len(errs) == 0 || errs[0].Code != "mt_unavailable" {
		t.Errorf("want an mt_unavailable error event, got %+v", errs)
	}
	// audio should still be produced (TTS speaks the passthrough text)
	if len(rec.byType("audio")) != 1 {
		t.Errorf("want audio in degraded mode")
	}
}
