package core

import (
	"fmt"
	"strings"
	"sync"
)

// ---- Fake STT -----------------------------------------------------------

// FakeSTT is an in-memory STT. Each configured utterance is revealed word by
// word as PARTIAL events over FeedsPerUtterance calls to Feed, then closed with
// a FINAL_EOU. Timestamps advance by MSPerFeed.
type FakeSTT struct {
	Utterances       []string
	FeedsPerUtterance int // default 1
	MSPerFeed        int64 // default 250
}

func (f *FakeSTT) NewStream(lang string) (STTStream, error) {
	fpu := f.FeedsPerUtterance
	if fpu < 1 {
		fpu = 1
	}
	ms := f.MSPerFeed
	if ms <= 0 {
		ms = 250
	}
	return &fakeSTTStream{utterances: f.Utterances, feedsPer: fpu, msPerFeed: ms}, nil
}

func (f *FakeSTT) Close() {}

type fakeSTTStream struct {
	mu         sync.Mutex
	utterances []string
	idx        int // current utterance
	feed       int // feeds into current utterance
	feedsPer   int
	msPerFeed  int64
	clockMS    int64
	uttStartMS int64
}

func (s *fakeSTTStream) Feed(pcm []float32) ([]STTEvent, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.idx >= len(s.utterances) {
		s.clockMS += s.msPerFeed
		return nil, nil
	}
	if s.feed == 0 {
		s.uttStartMS = s.clockMS
	}
	s.feed++
	s.clockMS += s.msPerFeed

	full := s.utterances[s.idx]
	words := strings.Fields(full)
	var evs []STTEvent
	if s.feed >= s.feedsPer {
		evs = append(evs, STTEvent{Kind: STTFinalEOU, Text: full, StartMS: s.uttStartMS, EndMS: s.clockMS})
		s.idx++
		s.feed = 0
	} else {
		n := len(words) * s.feed / s.feedsPer
		if n < 1 {
			n = 1
		}
		evs = append(evs, STTEvent{
			Kind: STTPartial, Text: strings.Join(words[:n], " "),
			StartMS: s.uttStartMS, EndMS: s.clockMS,
		})
	}
	return evs, nil
}

func (s *fakeSTTStream) Finish() ([]STTEvent, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var evs []STTEvent
	// flush any not-yet-final utterance
	if s.idx < len(s.utterances) {
		full := s.utterances[s.idx]
		evs = append(evs, STTEvent{Kind: STTFinalEOU, Text: full, StartMS: s.uttStartMS, EndMS: s.clockMS})
		s.idx++
		s.feed = 0
	}
	return evs, nil
}

func (s *fakeSTTStream) Close() {}

// ---- Fake MT ----------------------------------------------------------

// FakeMT is an in-memory translator. Translate delegates to Fn, or, if Fn is
// nil, returns "<dst>:<text>".
type FakeMT struct {
	Fn func(text, src, dst string) (string, error)
}

func (f *FakeMT) NewCtx() (MTCtx, error) { return &fakeMTCtx{fn: f.Fn}, nil }
func (f *FakeMT) Close()                 {}

type fakeMTCtx struct {
	fn func(text, src, dst string) (string, error)
}

func (c *fakeMTCtx) Translate(text, src, dst string) (string, error) {
	if c.fn != nil {
		return c.fn(text, src, dst)
	}
	return fmt.Sprintf("%s:%s", dst, text), nil
}
func (c *fakeMTCtx) Close() {}

// ---- Fake TTS --------------------------------------------------------

// FakeTTS is an in-memory synthesizer. Speak returns SamplesPerRune*len(text)
// samples (a ramp, so tests can assert non-silence) at SampleRate, unless Fn is
// set. Langs is what Languages() reports.
type FakeTTS struct {
	Langs         []string
	SampleRate    int // default 22050
	SamplesPerRune int // default 200
	Fn            func(text, lang, voice string) ([]float32, int, error)
}

func (f *FakeTTS) NewCtx() (TTSCtx, error) {
	sr := f.SampleRate
	if sr <= 0 {
		sr = 22050
	}
	spr := f.SamplesPerRune
	if spr <= 0 {
		spr = 200
	}
	return &fakeTTSCtx{fn: f.Fn, sampleRate: sr, samplesPerRune: spr, langs: f.langSet()}, nil
}

func (f *FakeTTS) langSet() map[string]bool {
	m := map[string]bool{}
	for _, l := range f.Languages() {
		m[l] = true
	}
	return m
}

func (f *FakeTTS) Languages() []string {
	if f.Langs != nil {
		return f.Langs
	}
	return append([]string(nil), VITSLangs...)
}

func (f *FakeTTS) Close() {}

type fakeTTSCtx struct {
	fn             func(text, lang, voice string) ([]float32, int, error)
	sampleRate     int
	samplesPerRune int
	langs          map[string]bool
}

func (c *fakeTTSCtx) Speak(text, lang, voice string) ([]float32, int, error) {
	if c.fn != nil {
		return c.fn(text, lang, voice)
	}
	if len(c.langs) > 0 && !c.langs[lang] {
		return nil, 0, ErrUnsupported
	}
	n := len([]rune(text)) * c.samplesPerRune
	if n == 0 {
		n = c.samplesPerRune
	}
	pcm := make([]float32, n)
	for i := range pcm {
		pcm[i] = float32(i%128) / 128.0
	}
	return pcm, c.sampleRate, nil
}
func (c *fakeTTSCtx) Close() {}
