//go:build cgo

package core

/*
#cgo CFLAGS: -I${SRCDIR}/../../../cores/common -I${SRCDIR}/../../../cores/stt -I${SRCDIR}/../../../cores/mt -I${SRCDIR}/../../../cores/tts_magpie
#cgo linux LDFLAGS: -ldl
#include <stdlib.h>
#include "shim.h"
*/
import "C"

import (
	"fmt"
	"strings"
	"sync"
	"unsafe"
)

// cgoAvailable reports whether this build can load native cores.
const cgoAvailable = true

func libPath(dir, base string) string {
	return dir + "/" + base + "." + nativeLibExt
}

// Load opens every configured core. Failures are recorded in Set.report and do
// not abort the others (degraded operation, per spec).
func Load(o Options) *Set {
	if o.MTCtx <= 0 {
		o.MTCtx = 512
	}
	if o.Device == "" {
		o.Device = "auto"
	}
	s := &Set{report: map[string]string{
		"stt": "not configured", "mt": "not configured",
		"tts_magpie": "not configured", "tts_vits": "not configured",
	}}

	if o.STTModel != "" {
		if stt, err := loadNativeSTT(libPath(o.LibDir, "libsb_stt"), o.STTModel, o.Device); err != nil {
			s.report["stt"] = err.Error()
		} else {
			s.STT, s.report["stt"] = stt, "ok"
		}
	}
	if o.MTModel != "" {
		if mt, err := loadNativeMT(libPath(o.LibDir, "libsb_mt"), o.MTModel, o.MTCtx); err != nil {
			s.report["mt"] = err.Error()
		} else {
			s.MT, s.report["mt"] = mt, "ok"
		}
	}
	if o.MagpieModel != "" {
		if t, err := loadNativeTTS(EngineMagpie, libPath(o.LibDir, "libsb_tts_magpie"), o.MagpieModel, o.Device); err != nil {
			s.report["tts_magpie"] = err.Error()
		} else {
			s.TTSMagpie, s.report["tts_magpie"] = t, "ok"
		}
	}
	if o.VITSDir != "" {
		if t, err := loadNativeTTS(EngineVITS, libPath(o.LibDir, "libsb_tts_vits"), o.VITSDir, o.Device); err != nil {
			s.report["tts_vits"] = err.Error()
		} else {
			s.TTSVits, s.report["tts_vits"] = t, "ok"
		}
	}
	return s
}

// ---- STT ----

type nativeSTT struct{ m *C.sb_stt_model }

func loadNativeSTT(libPath, modelPath, device string) (STT, error) {
	if err := openLib(openSTT, libPath, "stt"); err != nil {
		return nil, err
	}
	cp := C.CString(modelPath)
	cd := C.CString(device)
	defer C.free(unsafe.Pointer(cp))
	defer C.free(unsafe.Pointer(cd))
	m := C.sbn_stt_model_load(cp, cd)
	if m == nil {
		return nil, fmt.Errorf("%w: sb_stt_model_load(%s)", ErrModelLoad, modelPath)
	}
	return &nativeSTT{m: m}, nil
}

func (n *nativeSTT) NewStream(lang string) (STTStream, error) {
	cl := C.CString(lang)
	defer C.free(unsafe.Pointer(cl))
	s := C.sbn_stt_stream_new(n.m, cl)
	if s == nil {
		return nil, fmt.Errorf("%w: sb_stt_stream_new: %s", ErrModelLoad,
			C.GoString(C.sbn_stt_model_last_error(n.m)))
	}
	return &nativeSTTStream{s: s}, nil
}

func (n *nativeSTT) Close() {
	if n.m != nil {
		C.sbn_stt_model_free(n.m)
		n.m = nil
	}
}

type nativeSTTStream struct {
	mu sync.Mutex
	s  *C.sb_stt_stream
}

func (st *nativeSTTStream) Feed(pcm []float32) ([]STTEvent, error) {
	st.mu.Lock()
	defer st.mu.Unlock()
	var p *C.float
	if len(pcm) > 0 {
		p = (*C.float)(unsafe.Pointer(&pcm[0]))
	}
	if rc := C.sbn_stt_feed(st.s, p, C.size_t(len(pcm))); rc != 0 {
		return nil, fmt.Errorf("%w: sb_stt_feed: %s", statusErr(int(rc)),
			C.GoString(C.sbn_stt_last_error(st.s)))
	}
	return st.drain()
}

func (st *nativeSTTStream) Finish() ([]STTEvent, error) {
	st.mu.Lock()
	defer st.mu.Unlock()
	if rc := C.sbn_stt_finish(st.s); rc != 0 {
		return nil, fmt.Errorf("%w: sb_stt_finish: %s", statusErr(int(rc)),
			C.GoString(C.sbn_stt_last_error(st.s)))
	}
	return st.drain()
}

func (st *nativeSTTStream) drain() ([]STTEvent, error) {
	var evs []STTEvent
	for {
		var ev C.sb_stt_event
		if rc := C.sbn_stt_poll(st.s, &ev); rc != 0 {
			return evs, statusErr(int(rc))
		}
		if ev.kind == C.SB_STT_NONE {
			return evs, nil
		}
		evs = append(evs, STTEvent{
			Kind:    STTEventKind(ev.kind),
			Text:    C.GoString(ev.text), // copied out immediately (constraint 6)
			StartMS: int64(ev.start_ms),
			EndMS:   int64(ev.end_ms),
		})
	}
}

func (st *nativeSTTStream) Close() {
	st.mu.Lock()
	defer st.mu.Unlock()
	if st.s != nil {
		C.sbn_stt_stream_free(st.s)
		st.s = nil
	}
}

// ---- MT ----

type nativeMT struct {
	m     *C.sb_mt_model
	nCtx  int
}

func loadNativeMT(libPath, modelPath string, nCtx int) (MT, error) {
	if err := openLib(openMT, libPath, "mt"); err != nil {
		return nil, err
	}
	cp := C.CString(modelPath)
	defer C.free(unsafe.Pointer(cp))
	m := C.sbn_mt_model_load(cp, C.int(nCtx))
	if m == nil {
		return nil, fmt.Errorf("%w: sb_mt_model_load(%s)", ErrModelLoad, modelPath)
	}
	return &nativeMT{m: m, nCtx: nCtx}, nil
}

func (n *nativeMT) NewCtx() (MTCtx, error) {
	c := C.sbn_mt_ctx_new(n.m)
	if c == nil {
		return nil, fmt.Errorf("%w: sb_mt_ctx_new: %s", ErrModelLoad,
			C.GoString(C.sbn_mt_model_last_error(n.m)))
	}
	return &nativeMTCtx{c: c}, nil
}

func (n *nativeMT) Close() {
	if n.m != nil {
		C.sbn_mt_model_free(n.m)
		n.m = nil
	}
}

type nativeMTCtx struct {
	mu sync.Mutex
	c  *C.sb_mt_ctx
}

func (m *nativeMTCtx) Translate(text, src, dst string) (string, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	ct := C.CString(text)
	cs := C.CString(src)
	cd := C.CString(dst)
	defer C.free(unsafe.Pointer(ct))
	defer C.free(unsafe.Pointer(cs))
	defer C.free(unsafe.Pointer(cd))

	const cap = 16384
	buf := (*C.char)(C.malloc(cap))
	defer C.free(unsafe.Pointer(buf))

	rc := C.sbn_mt_translate(m.c, ct, cs, cd, buf, C.size_t(cap))
	if rc != 0 {
		return "", fmt.Errorf("%w: sb_mt_translate: %s", statusErr(int(rc)),
			C.GoString(C.sbn_mt_last_error(m.c)))
	}
	return C.GoString(buf), nil
}

func (m *nativeMTCtx) Close() {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.c != nil {
		C.sbn_mt_ctx_free(m.c)
		m.c = nil
	}
}

// ---- TTS ----

type nativeTTS struct {
	engine C.int // 0 magpie, 1 vits
	m      *C.sb_tts_model
	langs  []string
}

func loadNativeTTS(eng TTSEngine, libPath, modelPath, device string) (TTS, error) {
	var engine C.int
	var which openFn
	name := string(eng)
	switch eng {
	case EngineMagpie:
		engine, which = 0, openMagpie
	case EngineVITS:
		engine, which = 1, openVITS
	default:
		return nil, fmt.Errorf("core: unknown tts engine %q", eng)
	}
	if err := openLib(which, libPath, name); err != nil {
		return nil, err
	}
	cp := C.CString(modelPath)
	cd := C.CString(device)
	defer C.free(unsafe.Pointer(cp))
	defer C.free(unsafe.Pointer(cd))
	m := C.sbn_tts_model_load(engine, cp, cd)
	if m == nil {
		return nil, fmt.Errorf("%w: sb_tts_model_load(%s, %s)", ErrModelLoad, eng, modelPath)
	}
	n := &nativeTTS{engine: engine, m: m}
	n.langs = n.queryLangs()
	return n, nil
}

func (n *nativeTTS) queryLangs() []string {
	const cap = 512
	buf := (*C.char)(C.malloc(cap))
	defer C.free(unsafe.Pointer(buf))
	if C.sbn_tts_languages(n.engine, n.m, buf, C.size_t(cap)) != 0 {
		return nil
	}
	csv := C.GoString(buf)
	if csv == "" {
		return nil
	}
	parts := strings.Split(csv, ",")
	for i := range parts {
		parts[i] = strings.TrimSpace(parts[i])
	}
	return parts
}

func (n *nativeTTS) Languages() []string { return append([]string(nil), n.langs...) }

func (n *nativeTTS) NewCtx() (TTSCtx, error) {
	c := C.sbn_tts_ctx_new(n.engine, n.m)
	if c == nil {
		return nil, fmt.Errorf("%w: sb_tts_ctx_new: %s", ErrModelLoad,
			C.GoString(C.sbn_tts_model_last_error(n.engine, n.m)))
	}
	return &nativeTTSCtx{engine: n.engine, c: c}, nil
}

func (n *nativeTTS) Close() {
	if n.m != nil {
		C.sbn_tts_model_free(n.engine, n.m)
		n.m = nil
	}
}

type nativeTTSCtx struct {
	mu     sync.Mutex
	engine C.int
	c      *C.sb_tts_ctx
}

func (t *nativeTTSCtx) Speak(text, lang, voice string) ([]float32, int, error) {
	t.mu.Lock()
	defer t.mu.Unlock()
	ct := C.CString(text)
	cl := C.CString(lang)
	cv := C.CString(voice)
	defer C.free(unsafe.Pointer(ct))
	defer C.free(unsafe.Pointer(cl))
	defer C.free(unsafe.Pointer(cv))

	var pcm *C.float
	var n C.size_t
	var sr C.int
	rc := C.sbn_tts_speak(t.engine, t.c, ct, cl, cv, &pcm, &n, &sr)
	if rc != 0 {
		return nil, 0, fmt.Errorf("%w: sb_tts_speak: %s", statusErr(int(rc)),
			C.GoString(C.sbn_tts_last_error(t.engine, t.c)))
	}
	// Copy the C buffer out before it is invalidated by the next call.
	out := make([]float32, int(n))
	if n > 0 {
		src := unsafe.Slice((*float32)(unsafe.Pointer(pcm)), int(n))
		copy(out, src)
	}
	return out, int(sr), nil
}

func (t *nativeTTSCtx) Close() {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.c != nil {
		C.sbn_tts_ctx_free(t.engine, t.c)
		t.c = nil
	}
}

type openFn int

const (
	openSTT openFn = iota
	openMT
	openMagpie
	openVITS
)

// openLib dlopens one core library (cgo cannot pass C function pointers as Go
// values, so the target is selected here). rc 1 ("no path") is an error because
// callers only pass real paths.
func openLib(which openFn, path, name string) error {
	cp := C.CString(path)
	defer C.free(unsafe.Pointer(cp))
	var errbuf [512]C.char
	var rc C.int
	switch which {
	case openSTT:
		rc = C.sbn_open_stt(cp, &errbuf[0], C.size_t(len(errbuf)))
	case openMT:
		rc = C.sbn_open_mt(cp, &errbuf[0], C.size_t(len(errbuf)))
	case openMagpie:
		rc = C.sbn_open_tts_magpie(cp, &errbuf[0], C.size_t(len(errbuf)))
	case openVITS:
		rc = C.sbn_open_tts_vits(cp, &errbuf[0], C.size_t(len(errbuf)))
	}
	switch rc {
	case 0:
		return nil
	case 1:
		return fmt.Errorf("core %s: no library path", name)
	default:
		return fmt.Errorf("core %s: dlopen %s: %s", name, path, C.GoString(&errbuf[0]))
	}
}
