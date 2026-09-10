// Package httpapi serves the Speech Bridge batch REST API, the health/ready/
// metrics endpoints, /v1/capabilities and the embedded web UI. There are no
// HTTP calls between pipeline stages — this package only fronts the in-process
// pipeline.
package httpapi

import (
	"bufio"
	"context"
	"crypto/rand"
	"encoding/hex"
	"log/slog"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/cgouz/speech-bridge/app/internal/config"
	"github.com/cgouz/speech-bridge/app/internal/obs"
	"github.com/cgouz/speech-bridge/app/internal/pipeline"
)

// Deps are what the server needs from the rest of the process.
type Deps struct {
	Config    *config.Config
	Pipeline  *pipeline.Pipeline
	Logger    *slog.Logger
	Metrics   *obs.Metrics
	CoreState map[string]string // core name -> "ok" | error
	WebFS     http.Handler      // embedded web UI (may be nil)
	Stream    http.Handler      // /v1/stream WebSocket handler (may be nil)
}

// Server wraps an *http.Server plus the request queue.
type Server struct {
	deps Deps
	http *http.Server
	sem  chan struct{} // request-queue admission
}

// New builds the Server and its routes.
func New(d Deps) *Server {
	s := &Server{
		deps: d,
		sem:  make(chan struct{}, max1(d.Config.QueueDepth)),
	}
	mux := http.NewServeMux()

	// Liveness / readiness / metrics — never queued or authed.
	mux.HandleFunc("GET /health", s.handleHealth)
	mux.HandleFunc("GET /ready", s.handleReady)
	mux.HandleFunc("GET /metrics", s.handleMetrics)
	mux.HandleFunc("GET /v1/capabilities", s.handleCapabilities)

	// Batch API — authed + queued.
	mux.Handle("POST /v1/speech-to-speech", s.guard(http.HandlerFunc(s.handleSpeechToSpeech)))
	mux.Handle("POST /v1/transcribe", s.guard(http.HandlerFunc(s.handleTranscribe)))
	mux.Handle("POST /v1/translate", s.guard(http.HandlerFunc(s.handleTranslate)))
	mux.Handle("POST /v1/speak", s.guard(http.HandlerFunc(s.handleSpeak)))

	// Streaming WebSocket (authed in the handler; not queued). GET — the
	// upgrade request is a GET.
	if d.Stream != nil {
		mux.Handle("GET /v1/stream", s.auth(d.Stream))
	}

	// Web UI — exact root plus any other static path.
	if d.WebFS != nil {
		mux.Handle("GET /{$}", d.WebFS)
		mux.Handle("GET /index.html", d.WebFS)
	}

	s.http = &http.Server{
		Addr:              d.Config.Bind,
		Handler:           s.baseMiddleware(mux),
		ReadHeaderTimeout: 10 * time.Second,
	}
	return s
}

// Start listens and serves until the context is cancelled, then drains.
func (s *Server) Start(ctx context.Context) error {
	errCh := make(chan error, 1)
	go func() {
		s.deps.Logger.Info("http listening", "addr", s.http.Addr)
		if err := s.http.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			errCh <- err
		}
	}()
	select {
	case err := <-errCh:
		return err
	case <-ctx.Done():
		sc, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		return s.http.Shutdown(sc)
	}
}

// ---- middleware -------------------------------------------------------

func (s *Server) baseMiddleware(next http.Handler) http.Handler {
	maxBody := int64(s.deps.Config.MaxBodyMB) << 20
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		rid := newID()
		w.Header().Set("X-Request-ID", rid)
		ctx := obs.WithRequestID(r.Context(), rid)
		r = r.WithContext(ctx)

		if r.Body != nil && r.Method == http.MethodPost {
			r.Body = http.MaxBytesReader(w, r.Body, maxBody)
		}

		sw := &statusWriter{ResponseWriter: w, status: 200}
		defer func() {
			if rec := recover(); rec != nil {
				obs.From(ctx, s.deps.Logger).Error("panic", "err", rec, "path", r.URL.Path)
				if !sw.wrote {
					writeError(sw, http.StatusInternalServerError, "internal", "internal error", "", rid)
				}
			}
			s.deps.Metrics.CounterAdd("sb_requests_total", map[string]string{
				"endpoint": routeLabel(r.URL.Path), "status": strconv.Itoa(sw.status),
			}, 1)
		}()
		next.ServeHTTP(sw, r)
	})
}

// auth enforces Bearer auth when SB_AUTH_TOKEN is set.
func (s *Server) auth(next http.Handler) http.Handler {
	token := s.deps.Config.AuthToken
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if token != "" {
			got := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
			if got != token {
				writeError(w, http.StatusUnauthorized, "unauthorized", "missing or invalid bearer token", "", ridOf(r))
				return
			}
		}
		next.ServeHTTP(w, r)
	})
}

// guard = auth + queue admission (429 + Retry-After when full).
func (s *Server) guard(next http.Handler) http.Handler {
	return s.auth(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		select {
		case s.sem <- struct{}{}:
			defer func() { <-s.sem }()
			s.deps.Metrics.GaugeSet("sb_queue_depth", map[string]string{"core": "http"}, float64(len(s.sem)))
			ctx, cancel := context.WithTimeout(r.Context(), 180*time.Second)
			defer cancel()
			next.ServeHTTP(w, r.WithContext(ctx))
		default:
			w.Header().Set("Retry-After", "2")
			writeError(w, http.StatusTooManyRequests, "queue_full", "server busy, retry shortly", "", ridOf(r))
		}
	}))
}

// ---- helpers ---------------------------------------------------------

type statusWriter struct {
	http.ResponseWriter
	status int
	wrote  bool
}

func (w *statusWriter) WriteHeader(code int) {
	w.status = code
	w.wrote = true
	w.ResponseWriter.WriteHeader(code)
}
func (w *statusWriter) Write(b []byte) (int, error) {
	w.wrote = true
	return w.ResponseWriter.Write(b)
}

// Hijack forwards to the underlying writer so the WebSocket upgrade works
// through the middleware chain.
func (w *statusWriter) Hijack() (net.Conn, *bufio.ReadWriter, error) {
	if hj, ok := w.ResponseWriter.(http.Hijacker); ok {
		return hj.Hijack()
	}
	return nil, nil, http.ErrNotSupported
}

func (w *statusWriter) Flush() {
	if f, ok := w.ResponseWriter.(http.Flusher); ok {
		f.Flush()
	}
}

func newID() string {
	var b [12]byte
	_, _ = rand.Read(b[:])
	return hex.EncodeToString(b[:])
}

func ridOf(r *http.Request) string { return obs.RequestID(r.Context()) }

func max1(n int) int {
	if n < 1 {
		return 1
	}
	return n
}

func routeLabel(path string) string {
	switch {
	case path == "/health", path == "/ready", path == "/metrics":
		return path
	case strings.HasPrefix(path, "/v1/"):
		return path
	default:
		return "/"
	}
}
