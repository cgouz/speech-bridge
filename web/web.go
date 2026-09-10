// Package web embeds the Speech Bridge test UI (mic -> WS -> captions + audio).
// The whole app ships as one binary + four libs + models.
package web

import (
	"embed"
	"io/fs"
	"net/http"
)

//go:embed index.html
var files embed.FS

// Handler serves the embedded UI at "/".
func Handler() http.Handler {
	sub, _ := fs.Sub(files, ".")
	return http.FileServer(http.FS(sub))
}
