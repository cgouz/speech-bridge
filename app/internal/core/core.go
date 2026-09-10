// Package core loads the four Speech Bridge engine libraries (libsb_stt,
// libsb_mt, libsb_tts_magpie, libsb_tts_vits) via cgo + dlopen(RTLD_LOCAL) and
// exposes them behind small Go interfaces. In-memory fakes implementing the
// same interfaces let the pipeline and its tests run with no native libs and
// no models.
package core

import "errors"

// Errors surfaced across the ABI. The C side returns negative sb_status codes;
// these are their Go mirror.
var (
	ErrBadArg      = errors.New("core: bad argument")
	ErrModelLoad   = errors.New("core: model load failed")
	ErrInference   = errors.New("core: inference failed")
	ErrUnsupported = errors.New("core: unsupported language")
	ErrBusy        = errors.New("core: context busy")
	ErrNotLoaded   = errors.New("core: engine not loaded")
)

func statusErr(code int) error {
	switch code {
	case 0:
		return nil
	case -1:
		return ErrBadArg
	case -2:
		return ErrModelLoad
	case -3:
		return ErrInference
	case -4:
		return ErrUnsupported
	case -5:
		return ErrBusy
	default:
		return ErrInference
	}
}

// STTEventKind mirrors SB_STT_* in cores/stt/sb_stt.h.
type STTEventKind int

const (
	STTNone STTEventKind = iota
	STTPartial
	STTFinalEOU
)

func (k STTEventKind) String() string {
	switch k {
	case STTPartial:
		return "partial"
	case STTFinalEOU:
		return "final_eou"
	default:
		return "none"
	}
}

// STTEvent is one drained streaming event. Text is already copied out of C.
type STTEvent struct {
	Kind    STTEventKind
	Text    string
	StartMS int64
	EndMS   int64
}

// STT is a loaded streaming speech-to-text model. Weights load once; streams
// are cheap and hold per-session encoder/decoder cache.
type STT interface {
	NewStream(lang string) (STTStream, error)
	Close()
}

// STTStream is one speaker session. Not safe for concurrent use — the caller
// serializes Feed/Finish/Close (one inference at a time per context).
type STTStream interface {
	// Feed appends 16 kHz mono PCM and returns any events that became ready.
	Feed(pcm []float32) ([]STTEvent, error)
	// Finish flushes the end-of-audio tail and returns any final events.
	Finish() ([]STTEvent, error)
	Close()
}

// MT is a loaded translation model (MADLAD-400). Contexts are cheap.
type MT interface {
	NewCtx() (MTCtx, error)
	Close()
}

// MTCtx translates ONE sentence per call. Not safe for concurrent use.
type MTCtx interface {
	// Translate renders text from src ("auto" allowed) into dst; both are
	// language codes. Returns the translation (Cyrillic for dst=="uz").
	Translate(text, src, dst string) (string, error)
	Close()
}

// TTS is a loaded text-to-speech model — either magpie or vits, same interface.
type TTS interface {
	NewCtx() (TTSCtx, error)
	// Languages returns the language codes this engine can synthesize.
	Languages() []string
	Close()
}

// TTSCtx synthesizes ONE sentence per call. Not safe for concurrent use.
type TTSCtx interface {
	// Speak returns mono float32 PCM and its sample rate. The C buffer is
	// copied out before return.
	Speak(text, lang, voice string) (pcm []float32, sampleRate int, err error)
	Close()
}

// Engine role for a TTS instance, used by the pipeline's language router.
type TTSEngine string

const (
	EngineMagpie TTSEngine = "magpie"
	EngineVITS   TTSEngine = "vits"
)

// magpieLangs / vitsLangs are the mission's fixed routing table. The actual
// per-engine coverage reported to clients is the intersection of this with
// what each model's sb_tts_languages() and loaded state report.
var (
	MagpieLangs = []string{"en", "de", "es", "fr", "it", "pt-BR", "hi", "ko", "vi", "ar"}
	VITSLangs   = []string{"uz", "ru", "kaa"}
)

// Options configures which native cores to load and from where.
type Options struct {
	LibDir       string // directory holding libsb_*.{so,dylib}
	Device       string // "cpu" | "metal" | "auto"
	STTModel     string // SB_STT_MODEL   (empty = don't load STT)
	MTModel      string // SB_MT_MODEL
	MTCtx        int    // SB_MT_CTX (default 512)
	MagpieModel  string // SB_TTS_MAGPIE_MODEL
	VITSDir      string // SB_TTS_VITS_DIR
}

// Set is the loaded collection of cores. Any field may be nil (not configured
// or failed to load); Report explains why.
type Set struct {
	STT       STT
	MT        MT
	TTSMagpie TTS
	TTSVits   TTS

	report map[string]string // core name -> "ok" or an error string
}

// Report returns core name -> status ("ok" or an error message). Used by
// /ready and /v1/capabilities.
func (s *Set) Report() map[string]string {
	out := make(map[string]string, len(s.report))
	for k, v := range s.report {
		out[k] = v
	}
	return out
}

// Up reports whether the named core ("stt","mt","tts_magpie","tts_vits") loaded.
func (s *Set) Up(name string) bool { return s.report[name] == "ok" }

// Close frees every loaded context and model, then the libraries stay resident
// (dlclose is intentionally not called — matches the process lifetime).
func (s *Set) Close() {
	if s.STT != nil {
		s.STT.Close()
	}
	if s.MT != nil {
		s.MT.Close()
	}
	if s.TTSMagpie != nil {
		s.TTSMagpie.Close()
	}
	if s.TTSVits != nil {
		s.TTSVits.Close()
	}
}

// CGOEnabled reports whether this binary can load native cores at all.
func CGOEnabled() bool { return cgoAvailable }
