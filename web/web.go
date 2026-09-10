// Package web embeds the built Vue 3 frontend (see frontend/) so the whole
// app ships as one binary + four libs + models.
package web

import (
	"embed"
	"io/fs"
	"net/http"
	"path"
	"strings"
)

//go:embed all:dist
var files embed.FS

// Handler serves the embedded SPA. Static assets (JS/CSS/etc. under
// /assets/) are served as-is; any other path that isn't a real file falls
// back to index.html, so a refresh on a client-side route still works.
func Handler() http.Handler {
	sub, err := fs.Sub(files, "dist")
	if err != nil {
		panic(err) // dist/ is embedded at build time; always present
	}
	fileServer := http.FileServer(http.FS(sub))
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		p := strings.TrimPrefix(path.Clean(r.URL.Path), "/")
		if p == "" || p == "." {
			p = "index.html"
		}
		if _, err := fs.Stat(sub, p); err != nil {
			r2 := r.Clone(r.Context())
			r2.URL.Path = "/"
			fileServer.ServeHTTP(w, r2)
			return
		}
		fileServer.ServeHTTP(w, r)
	})
}
