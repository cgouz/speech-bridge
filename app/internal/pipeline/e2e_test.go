//go:build e2e

package pipeline_test

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"

	"github.com/cgouz/speech-bridge/app/internal/config"
	"github.com/cgouz/speech-bridge/app/internal/core"
	"github.com/cgouz/speech-bridge/app/internal/pipeline"
)

// decodeWAVForTest handles the 16-bit PCM mono fixture WAVs only.
func decodeWAVForTest(t *testing.T, b []byte) ([]float32, int) {
	t.Helper()
	if len(b) < 44 || string(b[0:4]) != "RIFF" {
		t.Fatalf("not a wav")
	}
	rate := int(binary.LittleEndian.Uint32(b[24:28]))
	ch := int(binary.LittleEndian.Uint16(b[22:24]))
	// find data chunk
	pos := 12
	var data []byte
	for pos+8 <= len(b) {
		id := string(b[pos : pos+4])
		sz := int(binary.LittleEndian.Uint32(b[pos+4 : pos+8]))
		pos += 8
		if id == "data" {
			end := pos + sz
			if end > len(b) {
				end = len(b)
			}
			data = b[pos:end]
			break
		}
		pos += sz
	}
	n := len(data) / 2 / ch
	out := make([]float32, n)
	for i := 0; i < n; i++ {
		var acc float32
		for c := 0; c < ch; c++ {
			acc += float32(int16(binary.LittleEndian.Uint16(data[(i*ch+c)*2:]))) / 32768
		}
		out[i] = acc / float32(ch)
	}
	return out, rate
}

// End-to-end: real cores + real models. Run with `make test-e2e`. Skips cleanly
// when models are absent.
func TestEndToEndBatch(t *testing.T) {
	repo := repoRoot(t)
	os.Setenv("SB_LIB_DIR", filepath.Join(repo, "lib"))
	os.Setenv("SB_MODELS_DIR", filepath.Join(repo, "models"))

	cfg, err := config.Load()
	if err != nil {
		t.Fatalf("config: %v", err)
	}
	if cfg.STTModel == "" {
		t.Skip("no STT model under models/ — run scripts/fetch-models.sh")
	}

	set := core.Load(core.Options{
		LibDir: cfg.LibDir, Device: cfg.Device,
		STTModel: cfg.STTModel, MTModel: cfg.MTModel, MTCtx: cfg.MTCtx,
		MagpieModel: cfg.MagpieModel, VITSDir: cfg.VITSDir,
	})
	defer set.Close()
	if !set.Up("stt") {
		t.Fatalf("stt core did not load: %s", set.Report()["stt"])
	}

	p, err := pipeline.New(pipeline.Engines{
		STT: set.STT, MT: set.MT, Magpie: set.TTSMagpie, Vits: set.TTSVits,
	})
	if err != nil {
		t.Fatal(err)
	}
	defer p.Close()

	wavPath := filepath.Join(repo, "third_party", "parakeet.cpp", "tests", "fixtures", "speech.wav")
	raw, err := os.ReadFile(wavPath)
	if err != nil {
		t.Skipf("no fixture wav: %v", err)
	}
	samples, rate := decodeWAVForTest(t, raw)

	res, err := p.Batch(pipeline.BatchInput{
		Audio: samples, SampleRate: rate,
		SourceLang: "en", TargetLang: "ru", Voice: "",
	})
	if err != nil {
		t.Fatalf("Batch: %v", err)
	}
	t.Logf("stage=%s transcript=%q translation=%q timings=%+v audio=%d@%dHz",
		res.Stage, res.Transcript, res.Translation, res.Timings, len(res.Audio), res.SampleRate)

	if res.Transcript == "" {
		t.Errorf("expected a non-empty transcript from the fixture clip")
	}
}

func repoRoot(t *testing.T) string {
	wd, _ := os.Getwd() // .../app/internal/pipeline
	return filepath.Clean(filepath.Join(wd, "..", "..", ".."))
}
