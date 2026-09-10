// Command sb-server is the Speech Bridge server: one process that dlopens the
// four core libraries and runs the streaming + batch speech-to-speech pipeline.
package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"github.com/cgouz/speech-bridge/app/internal/config"
	"github.com/cgouz/speech-bridge/app/internal/core"
	"github.com/cgouz/speech-bridge/app/internal/httpapi"
	"github.com/cgouz/speech-bridge/app/internal/obs"
	"github.com/cgouz/speech-bridge/app/internal/pipeline"
	"github.com/cgouz/speech-bridge/app/internal/stream"
	"github.com/cgouz/speech-bridge/web"
)

var version = "0.1.0" // overridden via -ldflags at build time

func main() {
	showVersion := flag.Bool("version", false, "print version and exit")
	flag.Parse()
	if *showVersion {
		fmt.Println("sb-server", version)
		return
	}

	cfg, err := config.Load()
	if err != nil {
		fmt.Fprintln(os.Stderr, "sb-server: "+err.Error())
		os.Exit(2)
	}

	log := obs.NewLogger(cfg.LogFormat, cfg.LogLevel)
	metrics := obs.NewMetrics()

	log.Info("sb-server starting", "version", version, "bind", cfg.Bind,
		"cgo", core.CGOEnabled(), "lib_dir", cfg.LibDir, "models_dir", cfg.ModelsDir)

	set := core.Load(core.Options{
		LibDir:      cfg.LibDir,
		Device:      cfg.Device,
		STTModel:    cfg.STTModel,
		MTModel:     cfg.MTModel,
		MTCtx:       cfg.MTCtx,
		MagpieModel: cfg.MagpieModel,
		VITSDir:     cfg.VITSDir,
	})
	defer set.Close()
	for name, st := range set.Report() {
		if st == "ok" {
			log.Info("core loaded", "core", name)
		} else {
			log.Warn("core not available", "core", name, "reason", st)
		}
	}

	pipe, err := pipeline.New(pipeline.Engines{
		STT: set.STT, MT: set.MT, Magpie: set.TTSMagpie, Vits: set.TTSVits,
	})
	if err != nil {
		log.Error("pipeline init failed", "err", err)
		os.Exit(1)
	}
	defer pipe.Close()

	srv := httpapi.New(httpapi.Deps{
		Config:    cfg,
		Pipeline:  pipe,
		Logger:    log,
		Metrics:   metrics,
		CoreState: set.Report(),
		WebFS:     web.Handler(),
		Stream:    stream.Handler(pipe, log, metrics, cfg.StreamsMax),
	})

	ctx, stopSignals := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stopSignals()

	log.Info("ready")
	if err := srv.Start(ctx); err != nil {
		log.Error("server stopped with error", "err", err)
		os.Exit(1)
	}
	log.Info("shutdown complete")
}
