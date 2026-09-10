// Package stream is the /v1/stream WebSocket endpoint. It adapts WebSocket
// frames to a session.Session: one connection = one speaker -> one target
// language. JSON control frames + binary PCM frames in; JSON events and binary
// audio frames out.
package stream

import (
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"math"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	"github.com/coder/websocket"

	"github.com/cgouz/speech-bridge/app/internal/obs"
	"github.com/cgouz/speech-bridge/app/internal/pipeline"
	"github.com/cgouz/speech-bridge/app/internal/session"
)

const idleTimeout = 60 * time.Second

// Handler builds the /v1/stream HTTP handler.
func Handler(p *pipeline.Pipeline, log *slog.Logger, metrics *obs.Metrics, streamsMax int) http.Handler {
	var active int64
	if streamsMax < 1 {
		streamsMax = 4
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if n := atomic.AddInt64(&active, 1); n > int64(streamsMax) {
			atomic.AddInt64(&active, -1)
			http.Error(w, "too many streams", http.StatusServiceUnavailable)
			return
		}
		defer atomic.AddInt64(&active, -1)
		metrics.GaugeSet("sb_active_streams", nil, float64(atomic.LoadInt64(&active)))
		defer func() { metrics.GaugeSet("sb_active_streams", nil, float64(atomic.LoadInt64(&active)-1)) }()

		c, err := websocket.Accept(w, r, &websocket.AcceptOptions{})
		if err != nil {
			return
		}
		defer c.CloseNow()
		c.SetReadLimit(8 << 20)

		sid := obs.RequestID(r.Context())
		l := log.With("session_id", sid, "component", "stream")
		serve(r.Context(), c, p, l)
	})
}

func serve(ctx context.Context, c *websocket.Conn, p *pipeline.Pipeline, log *slog.Logger) {
	// first frame must be the JSON "start"
	sctx, cancel := context.WithTimeout(ctx, idleTimeout)
	typ, data, err := c.Read(sctx)
	cancel()
	if err != nil || typ != websocket.MessageText {
		_ = c.Close(websocket.StatusUnsupportedData, "expected start frame")
		return
	}
	var start session.Start
	if err := json.Unmarshal(data, &start); err != nil || start.Type != "start" {
		_ = c.Close(websocket.StatusUnsupportedData, "invalid start frame")
		return
	}
	if start.TargetLang == "" {
		_ = c.Close(websocket.StatusPolicyViolation, "target_lang required")
		return
	}

	var writeMu sync.Mutex
	send := func(m session.Msg) {
		writeMu.Lock()
		defer writeMu.Unlock()
		wctx, wcancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer wcancel()
		hdr, _ := json.Marshal(m)
		if err := c.Write(wctx, websocket.MessageText, hdr); err != nil {
			return
		}
		if m.Type == "audio" && len(m.Binary) > 0 {
			_ = c.Write(wctx, websocket.MessageBinary, floatsToLE(m.Binary))
		}
	}

	sess, err := session.New(p, start, send)
	if err != nil {
		send(session.Msg{Type: "error", Code: "stt_unavailable", Stage: "stt", Text: err.Error()})
		_ = c.Close(websocket.StatusInternalError, "stt unavailable")
		return
	}
	defer sess.Close()

	log.Info("stream started", "source_lang", start.SourceLang, "target_lang", start.TargetLang)

	for {
		rctx, rcancel := context.WithTimeout(ctx, idleTimeout)
		mtype, msg, err := c.Read(rctx)
		rcancel()
		if err != nil {
			if !errors.Is(err, context.Canceled) {
				log.Info("stream read end", "err", err)
			}
			break
		}
		switch mtype {
		case websocket.MessageBinary:
			sess.PushAudio(msg)
		case websocket.MessageText:
			var ctrl struct {
				Type string `json:"type"`
			}
			_ = json.Unmarshal(msg, &ctrl)
			if ctrl.Type == "stop" {
				sess.Stop()
				_ = c.Close(websocket.StatusNormalClosure, "")
				return
			}
		}
	}
	sess.Stop()
	_ = c.Close(websocket.StatusNormalClosure, "")
}

// floatsToLE encodes float32 PCM as little-endian bytes for the binary frame.
func floatsToLE(f []float32) []byte {
	b := make([]byte, len(f)*4)
	for i, v := range f {
		u := math.Float32bits(v)
		b[i*4] = byte(u)
		b[i*4+1] = byte(u >> 8)
		b[i*4+2] = byte(u >> 16)
		b[i*4+3] = byte(u >> 24)
	}
	return b
}
