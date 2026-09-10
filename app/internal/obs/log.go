// Package obs is Speech Bridge observability: slog JSON logging with
// request_id / session_id and a tiny dependency-free Prometheus registry.
//
// Transcript and translation text are logged ONLY at debug level — user speech
// is private data (mission rule).
package obs

import (
	"context"
	"log/slog"
	"os"
)

type ctxKey int

const (
	keyRequestID ctxKey = iota
	keySessionID
)

// NewLogger builds the process logger. format is "json" or "text"; level is
// "debug"|"info"|"warn"|"error".
func NewLogger(format, level string) *slog.Logger {
	var lv slog.Level
	switch level {
	case "debug":
		lv = slog.LevelDebug
	case "warn":
		lv = slog.LevelWarn
	case "error":
		lv = slog.LevelError
	default:
		lv = slog.LevelInfo
	}
	opts := &slog.HandlerOptions{Level: lv}
	var h slog.Handler
	if format == "text" {
		h = slog.NewTextHandler(os.Stdout, opts)
	} else {
		h = slog.NewJSONHandler(os.Stdout, opts)
	}
	return slog.New(h)
}

// WithRequestID / WithSessionID attach ids to a context.
func WithRequestID(ctx context.Context, id string) context.Context {
	return context.WithValue(ctx, keyRequestID, id)
}
func WithSessionID(ctx context.Context, id string) context.Context {
	return context.WithValue(ctx, keySessionID, id)
}

// RequestID returns the request id on the context, or "".
func RequestID(ctx context.Context) string {
	v, _ := ctx.Value(keyRequestID).(string)
	return v
}

// SessionID returns the session id on the context, or "".
func SessionID(ctx context.Context) string {
	v, _ := ctx.Value(keySessionID).(string)
	return v
}

// From returns a logger pre-tagged with whatever ids are on the context.
func From(ctx context.Context, base *slog.Logger) *slog.Logger {
	l := base
	if v, ok := ctx.Value(keyRequestID).(string); ok && v != "" {
		l = l.With("request_id", v)
	}
	if v, ok := ctx.Value(keySessionID).(string); ok && v != "" {
		l = l.With("session_id", v)
	}
	return l
}
