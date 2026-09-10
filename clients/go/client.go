// Package speechbridge is a Go SDK for sb-server: a batch client and a
// streaming WebSocket client behind one interface.
package speechbridge

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"strings"
)

// Client talks to a Speech Bridge server.
type Client struct {
	BaseURL string // e.g. "http://127.0.0.1:8080"
	Token   string // optional bearer token
	HTTP    *http.Client
}

// New returns a Client with sensible defaults.
func New(baseURL string) *Client {
	return &Client{BaseURL: strings.TrimRight(baseURL, "/"), HTTP: http.DefaultClient}
}

func (c *Client) do(ctx context.Context, method, path, ct string, body io.Reader) (*http.Response, error) {
	req, err := http.NewRequestWithContext(ctx, method, c.BaseURL+path, body)
	if err != nil {
		return nil, err
	}
	if ct != "" {
		req.Header.Set("Content-Type", ct)
	}
	if c.Token != "" {
		req.Header.Set("Authorization", "Bearer "+c.Token)
	}
	h := c.HTTP
	if h == nil {
		h = http.DefaultClient
	}
	return h.Do(req)
}

// APIError is a non-2xx response.
type APIError struct {
	Status int
	Code   string `json:"code"`
	Msg    string `json:"message"`
	Stage  string `json:"stage"`
}

func (e *APIError) Error() string {
	return fmt.Sprintf("speechbridge: %d %s: %s (stage=%s)", e.Status, e.Code, e.Msg, e.Stage)
}

func decode(resp *http.Response, out any) error {
	defer resp.Body.Close()
	data, _ := io.ReadAll(resp.Body)
	if resp.StatusCode/100 != 2 {
		var wrap struct {
			Error APIError `json:"error"`
		}
		_ = json.Unmarshal(data, &wrap)
		wrap.Error.Status = resp.StatusCode
		if wrap.Error.Code == "" {
			wrap.Error.Code = "http_error"
			wrap.Error.Msg = strings.TrimSpace(string(data))
		}
		return &wrap.Error
	}
	if out == nil {
		return nil
	}
	return json.Unmarshal(data, out)
}

// ---- health / capabilities ----

// Ready reports whether the server is ready to serve.
func (c *Client) Ready(ctx context.Context) (bool, error) {
	resp, err := c.do(ctx, http.MethodGet, "/ready", "", nil)
	if err != nil {
		return false, err
	}
	var body struct {
		Ready bool `json:"ready"`
	}
	if err := decode(resp, &body); err != nil {
		return false, err
	}
	return body.Ready, nil
}

// Capabilities describes what the server can do.
type Capabilities struct {
	Cores    map[string]string   `json:"cores"`
	TTS      map[string][]string `json:"tts"`
	STTLangs []string            `json:"stt_langs"`
}

func (c *Client) Capabilities(ctx context.Context) (*Capabilities, error) {
	resp, err := c.do(ctx, http.MethodGet, "/v1/capabilities", "", nil)
	if err != nil {
		return nil, err
	}
	var caps Capabilities
	return &caps, decode(resp, &caps)
}

// ---- batch ----

// Timings mirrors the server's per-stage timings.
type Timings struct {
	STTMs   int64 `json:"stt_ms"`
	MTMs    int64 `json:"mt_ms"`
	TTSMs   int64 `json:"tts_ms"`
	TotalMs int64 `json:"total_ms"`
}

// SpeechToSpeechResult is the batch pipeline output.
type SpeechToSpeechResult struct {
	RequestID   string  `json:"request_id"`
	Transcript  string  `json:"transcript"`
	Translation string  `json:"translation"`
	Audio       string  `json:"audio"` // base64 WAV
	AudioFormat string  `json:"audio_format"`
	SampleRate  int     `json:"sample_rate"`
	Timings     Timings `json:"timings"`
	Stage       string  `json:"stage"`
}

// SpeechToSpeech uploads a WAV and returns transcript + translation + audio.
func (c *Client) SpeechToSpeech(ctx context.Context, wav []byte, srcLang, dstLang, voice string) (*SpeechToSpeechResult, error) {
	var buf bytes.Buffer
	mw := multipart.NewWriter(&buf)
	_ = mw.WriteField("source_lang", srcLang)
	_ = mw.WriteField("target_lang", dstLang)
	if voice != "" {
		_ = mw.WriteField("voice", voice)
	}
	fw, _ := mw.CreateFormFile("file", "clip.wav")
	_, _ = fw.Write(wav)
	_ = mw.Close()

	resp, err := c.do(ctx, http.MethodPost, "/v1/speech-to-speech", mw.FormDataContentType(), &buf)
	if err != nil {
		return nil, err
	}
	var out SpeechToSpeechResult
	return &out, decode(resp, &out)
}

// Transcribe returns just the transcript for a WAV.
func (c *Client) Transcribe(ctx context.Context, wav []byte, srcLang string) (string, error) {
	resp, err := c.do(ctx, http.MethodPost, "/v1/transcribe?source_lang="+srcLang, "audio/wav", bytes.NewReader(wav))
	if err != nil {
		return "", err
	}
	var out struct {
		Transcript string `json:"transcript"`
	}
	return out.Transcript, decode(resp, &out)
}

// Translate returns the translation of text into dstLang.
func (c *Client) Translate(ctx context.Context, text, srcLang, dstLang string) (string, error) {
	body, _ := json.Marshal(map[string]string{"text": text, "source_lang": srcLang, "target_lang": dstLang})
	resp, err := c.do(ctx, http.MethodPost, "/v1/translate", "application/json", bytes.NewReader(body))
	if err != nil {
		return "", err
	}
	var out struct {
		Translation string `json:"translation"`
	}
	return out.Translation, decode(resp, &out)
}

// Speak synthesizes text and returns WAV bytes.
func (c *Client) Speak(ctx context.Context, text, lang, voice string) ([]byte, error) {
	body, _ := json.Marshal(map[string]string{"text": text, "lang": lang, "voice": voice})
	resp, err := c.do(ctx, http.MethodPost, "/v1/speak", "application/json", bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode/100 != 2 {
		return nil, decode(resp, nil)
	}
	return io.ReadAll(resp.Body)
}
