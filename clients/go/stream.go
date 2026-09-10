package speechbridge

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"math"
	"net/http"

	"github.com/coder/websocket"
)

// StreamEvent is a server->client message on /v1/stream. For "audio" events
// PCM holds the float32 samples of the frame that followed the header.
type StreamEvent struct {
	Type       string    `json:"type"`
	Text       string    `json:"text"`
	Seq        int       `json:"seq"`
	T0MS       int64     `json:"t0_ms"`
	T1MS       int64     `json:"t1_ms"`
	SampleRate int       `json:"sample_rate"`
	Code       string    `json:"code"`
	Stage      string    `json:"stage"`
	PCM        []float32 `json:"-"`
}

// Stream is a live speech-to-speech session.
type Stream struct {
	conn   *websocket.Conn
	ctx    context.Context
	events chan StreamEvent
	errc   chan error
}

// StartStream opens /v1/stream and sends the start frame. Feed PCM with Send,
// finish with Stop, read results from Events.
func (c *Client) StartStream(ctx context.Context, srcLang, dstLang, voice string, sampleRate int, format string) (*Stream, error) {
	wsURL := "ws" + c.BaseURL[len("http"):] + "/v1/stream"
	hdr := http.Header{}
	if c.Token != "" {
		hdr.Set("Authorization", "Bearer "+c.Token)
	}
	conn, _, err := websocket.Dial(ctx, wsURL, &websocket.DialOptions{HTTPHeader: hdr})
	if err != nil {
		return nil, err
	}
	if format == "" {
		format = "f32"
	}
	if sampleRate == 0 {
		sampleRate = 16000
	}
	start, _ := json.Marshal(map[string]any{
		"type": "start", "source_lang": srcLang, "target_lang": dstLang,
		"voice": voice, "sample_rate": sampleRate, "format": format,
	})
	if err := conn.Write(ctx, websocket.MessageText, start); err != nil {
		conn.CloseNow()
		return nil, err
	}

	s := &Stream{conn: conn, ctx: ctx, events: make(chan StreamEvent, 32), errc: make(chan error, 1)}
	go s.readLoop()
	return s, nil
}

func (s *Stream) readLoop() {
	defer close(s.events)
	var pendingAudio *StreamEvent
	for {
		typ, data, err := s.conn.Read(s.ctx)
		if err != nil {
			s.errc <- err
			return
		}
		if typ == websocket.MessageBinary {
			if pendingAudio != nil {
				pendingAudio.PCM = bytesToFloats(data)
				s.events <- *pendingAudio
				pendingAudio = nil
			}
			continue
		}
		var ev StreamEvent
		if json.Unmarshal(data, &ev) != nil {
			continue
		}
		if ev.Type == "audio" {
			e := ev
			pendingAudio = &e
			continue
		}
		s.events <- ev
		if ev.Type == "done" {
			return
		}
	}
}

// Send feeds a chunk of mono float32 PCM (at the stream's sample rate).
func (s *Stream) Send(pcm []float32) error {
	b := make([]byte, len(pcm)*4)
	for i, v := range pcm {
		binary.LittleEndian.PutUint32(b[i*4:], math.Float32bits(v))
	}
	return s.conn.Write(s.ctx, websocket.MessageBinary, b)
}

// Stop sends the stop frame; drain Events until it yields a "done".
func (s *Stream) Stop() error {
	stop, _ := json.Marshal(map[string]string{"type": "stop"})
	return s.conn.Write(s.ctx, websocket.MessageText, stop)
}

// Events is the channel of server messages; it closes when the stream ends.
func (s *Stream) Events() <-chan StreamEvent { return s.events }

// Err returns the terminating error, if any (available after Events closes).
func (s *Stream) Err() error {
	select {
	case e := <-s.errc:
		return e
	default:
		return nil
	}
}

// Close tears down the connection.
func (s *Stream) Close() error { return s.conn.Close(websocket.StatusNormalClosure, "") }

func bytesToFloats(b []byte) []float32 {
	out := make([]float32, len(b)/4)
	for i := range out {
		out[i] = math.Float32frombits(binary.LittleEndian.Uint32(b[i*4:]))
	}
	return out
}
