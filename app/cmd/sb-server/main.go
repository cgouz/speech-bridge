// Command sb-server is the Speech Bridge server: one process that dlopens the
// four core libraries and runs the streaming + batch speech-to-speech pipeline.
//
// Milestone 1: skeleton only — flag/version handling. Config validation,
// core loading, HTTP/WS servers and graceful shutdown land in milestones 3–8.
package main

import (
	"flag"
	"fmt"
	"os"
)

// version is stamped from the repo VERSION file at build time in later
// milestones; keep a literal default for now.
const version = "0.1.0"

func main() {
	showVersion := flag.Bool("version", false, "print version and exit")
	bind := flag.String("bind", envOr("SB_BIND", "127.0.0.1:8080"), "listen address")
	flag.Parse()

	if *showVersion {
		fmt.Println("sb-server", version)
		return
	}

	fmt.Fprintf(os.Stderr, "sb-server %s: not runnable yet (milestone 3+). bind=%s\n", version, *bind)
	os.Exit(1)
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}
