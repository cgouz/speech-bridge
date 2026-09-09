//go:build !cgo

package core

// This stub lets `CGO_ENABLED=0 go build/test` succeed for the pure-Go packages
// (text, config, protocol, pipeline-on-fakes). Load reports every core
// unavailable; callers fall back to fakes or degrade.

const cgoAvailable = false

// Load returns a Set with no native cores (cgo disabled at build time).
func Load(o Options) *Set {
	const msg = "native cores unavailable: built without cgo"
	return &Set{report: map[string]string{
		"stt": msg, "mt": msg, "tts_magpie": msg, "tts_vits": msg,
	}}
}
