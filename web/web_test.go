package web

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestHandlerServesIndexAtRoot(t *testing.T) {
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/", nil)
	Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("GET / = %d, want 200", rec.Code)
	}
	if !strings.Contains(rec.Body.String(), "<title>Speech Bridge</title>") {
		t.Fatalf("GET / body missing expected content: %s", rec.Body.String())
	}
}

func TestHandlerSPAFallback(t *testing.T) {
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/some/unknown/client/route", nil)
	Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("unknown route = %d, want 200 (SPA fallback)", rec.Code)
	}
	if !strings.Contains(rec.Body.String(), "<title>Speech Bridge</title>") {
		t.Fatalf("fallback body missing expected content: %s", rec.Body.String())
	}
}

func TestHandlerServesRealAsset(t *testing.T) {
	assets, err := files.ReadDir("dist/assets")
	if err != nil || len(assets) == 0 {
		t.Skip("no built assets under dist/assets (frontend not built)")
	}

	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/assets/"+assets[0].Name(), nil)
	Handler().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("GET /assets/%s = %d, want 200", assets[0].Name(), rec.Code)
	}
	if strings.Contains(rec.Body.String(), "<title>Speech Bridge</title>") {
		t.Fatalf("asset request was served the SPA fallback instead of the real file")
	}
}
