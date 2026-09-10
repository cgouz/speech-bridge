// Command sb-bench measures the Speech Bridge latency budget against a running
// sb-server and prints a table. Requires models loaded on the server.
//
//	sb-bench -addr 127.0.0.1:8080 -wav clip.wav -src ru -dst uz
package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"math"
	"mime/multipart"
	"net/http"
	"os"
	"time"

	"github.com/coder/websocket"
)

func main() {
	addr := flag.String("addr", "127.0.0.1:8080", "sb-server address")
	wav := flag.String("wav", "", "path to a WAV clip (mono; 16k or any rate)")
	src := flag.String("src", "ru", "source language")
	dst := flag.String("dst", "uz", "target language")
	token := flag.String("token", os.Getenv("SB_AUTH_TOKEN"), "bearer token")
	flag.Parse()

	if *wav == "" {
		fmt.Fprintln(os.Stderr, "sb-bench: -wav is required")
		os.Exit(2)
	}
	raw, err := os.ReadFile(*wav)
	if err != nil {
		fail(err)
	}
	samples, rate := decodeWAV(raw)
	clipSec := float64(len(samples)) / float64(rate)

	fmt.Printf("== Speech Bridge bench ==\n")
	fmt.Printf("server   : %s\n", *addr)
	fmt.Printf("clip     : %s  (%.1fs, %d Hz)\n", *wav, clipSec, rate)
	fmt.Printf("langs    : %s -> %s\n\n", *src, *dst)

	base := "http://" + *addr

	// ---- batch ----
	bStart := time.Now()
	bt, stage := batch(base, *token, raw, *src, *dst)
	bWall := time.Since(bStart)
	fmt.Printf("%-34s %8s\n", "batch end-to-end (wall)", dur(bWall))
	fmt.Printf("%-34s %8s\n", "  stt_ms", ms(bt.STT))
	fmt.Printf("%-34s %8s\n", "  mt_ms", ms(bt.MT))
	fmt.Printf("%-34s %8s\n", "  tts_ms", ms(bt.TTS))
	fmt.Printf("%-34s %8s   stage=%s\n\n", "  total_ms (server)", ms(bt.Total), stage)

	// ---- streaming ----
	st := streamBench(*addr, *token, samples, rate, *src, *dst)
	fmt.Printf("%-34s %8s\n", "stream: first partial after audio", dur(st.firstPartial))
	fmt.Printf("%-34s %8s\n", "stream: EOU -> translation", dur(st.eouToTranslation))
	fmt.Printf("%-34s %8s\n", "stream: EOU -> first audio", dur(st.eouToAudio))

	fmt.Printf("\n-- budget --\n")
	check("STT partial lag < 300 ms", st.firstPartial > 0 && st.firstPartial < 300*time.Millisecond)
	if st.eouToTranslation == 0 {
		fmt.Printf("  [n/a ] EOU -> translation < 2 s   (no MT engine loaded)\n")
	} else {
		check("EOU -> translation < 2 s", st.eouToTranslation < 2*time.Second)
	}
	check("EOU -> first audio < 4 s", st.eouToAudio > 0 && st.eouToAudio < 4*time.Second)
	check(fmt.Sprintf("batch %.0fs clip end-to-end < %.0fs", clipSec, clipSec*1.5), bWall < time.Duration(float64(time.Second)*clipSec*1.5))
	fmt.Printf("\nNote: CPU-only figures. On Apple Silicon with --metal (STT+MT), expect notably lower latency.\n")
}

type timings struct {
	STT   int64 `json:"stt_ms"`
	MT    int64 `json:"mt_ms"`
	TTS   int64 `json:"tts_ms"`
	Total int64 `json:"total_ms"`
}

func batch(base, token string, wav []byte, src, dst string) (timings, string) {
	var body bytes.Buffer
	mw := multipart.NewWriter(&body)
	mw.WriteField("source_lang", src)
	mw.WriteField("target_lang", dst)
	fw, _ := mw.CreateFormFile("file", "clip.wav")
	fw.Write(wav)
	mw.Close()

	req, _ := http.NewRequest("POST", base+"/v1/speech-to-speech", &body)
	req.Header.Set("Content-Type", mw.FormDataContentType())
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		fail(err)
	}
	defer resp.Body.Close()
	var out struct {
		Timings timings `json:"timings"`
		Stage   string  `json:"stage"`
	}
	json.NewDecoder(resp.Body).Decode(&out)
	return out.Timings, out.Stage
}

type streamResult struct {
	firstPartial     time.Duration
	eouToTranslation time.Duration
	eouToAudio       time.Duration
}

func streamBench(addr, token string, samples []float32, rate int, src, dst string) streamResult {
	ctx := context.Background()
	hdr := http.Header{}
	if token != "" {
		hdr.Set("Authorization", "Bearer "+token)
	}
	c, _, err := websocket.Dial(ctx, "ws://"+addr+"/v1/stream", &websocket.DialOptions{HTTPHeader: hdr})
	if err != nil {
		fail(err)
	}
	defer c.CloseNow()

	start, _ := json.Marshal(map[string]any{
		"type": "start", "source_lang": src, "target_lang": dst,
		"sample_rate": rate, "format": "f32",
	})
	c.Write(ctx, websocket.MessageText, start)

	var res streamResult
	audioStart := time.Now()
	var eouAt time.Time
	done := make(chan struct{})

	go func() {
		defer close(done)
		for {
			typ, data, err := c.Read(ctx)
			if err != nil {
				return
			}
			if typ != websocket.MessageText {
				if !eouAt.IsZero() && res.eouToAudio == 0 {
					res.eouToAudio = time.Since(eouAt)
				}
				continue
			}
			var m struct {
				Type string `json:"type"`
			}
			json.Unmarshal(data, &m)
			switch m.Type {
			case "partial":
				if res.firstPartial == 0 {
					res.firstPartial = time.Since(audioStart)
				}
			case "transcript":
				eouAt = time.Now()
			case "translation":
				if !eouAt.IsZero() && res.eouToTranslation == 0 {
					res.eouToTranslation = time.Since(eouAt)
				}
			case "audio":
				if !eouAt.IsZero() && res.eouToAudio == 0 {
					res.eouToAudio = time.Since(eouAt)
				}
			case "done":
				return
			}
		}
	}()

	// feed ~250 ms frames in real time
	const frame = 4000
	buf16k := resample(samples, rate, 16000)
	for off := 0; off < len(buf16k); off += frame {
		end := off + frame
		if end > len(buf16k) {
			end = len(buf16k)
		}
		c.Write(ctx, websocket.MessageBinary, floatsLE(buf16k[off:end]))
		time.Sleep(240 * time.Millisecond)
	}
	stop, _ := json.Marshal(map[string]string{"type": "stop"})
	c.Write(ctx, websocket.MessageText, stop)

	select {
	case <-done:
	case <-time.After(30 * time.Second):
	}
	return res
}

// ---- helpers ----

func decodeWAV(b []byte) ([]float32, int) {
	rate := int(binary.LittleEndian.Uint32(b[24:28]))
	ch := int(binary.LittleEndian.Uint16(b[22:24]))
	bits := int(binary.LittleEndian.Uint16(b[34:36]))
	pos := 12
	var data []byte
	for pos+8 <= len(b) {
		id := string(b[pos : pos+4])
		sz := int(binary.LittleEndian.Uint32(b[pos+4 : pos+8]))
		pos += 8
		if id == "data" {
			e := pos + sz
			if e > len(b) {
				e = len(b)
			}
			data = b[pos:e]
			break
		}
		pos += sz
	}
	step := bits / 8
	n := len(data) / step / ch
	out := make([]float32, n)
	for i := 0; i < n; i++ {
		var acc float32
		for c := 0; c < ch; c++ {
			off := (i*ch + c) * step
			if bits == 16 {
				acc += float32(int16(binary.LittleEndian.Uint16(data[off:]))) / 32768
			} else if bits == 32 {
				acc += math.Float32frombits(binary.LittleEndian.Uint32(data[off:]))
			}
		}
		out[i] = acc / float32(ch)
	}
	return out, rate
}

func resample(in []float32, from, to int) []float32 {
	if from == to || len(in) == 0 {
		return in
	}
	ratio := float64(from) / float64(to)
	out := make([]float32, int(float64(len(in))/ratio))
	for i := range out {
		j := int(float64(i) * ratio)
		if j >= len(in) {
			j = len(in) - 1
		}
		out[i] = in[j]
	}
	return out
}

func floatsLE(f []float32) []byte {
	b := make([]byte, len(f)*4)
	for i, v := range f {
		binary.LittleEndian.PutUint32(b[i*4:], math.Float32bits(v))
	}
	return b
}

func dur(d time.Duration) string { return fmt.Sprintf("%d ms", d.Milliseconds()) }
func ms(v int64) string          { return fmt.Sprintf("%d ms", v) }

func check(name string, ok bool) {
	mark := "FAIL"
	if ok {
		mark = "ok"
	}
	fmt.Printf("  [%-4s] %s\n", mark, name)
}

func fail(err error) {
	fmt.Fprintln(os.Stderr, "sb-bench:", err)
	os.Exit(1)
}
