# Operations

Running and deploying `sb-server`.

## Layout on a host

```
/opt/speechbridge/
├── sb-server                 # the C++ binary
├── lib/                      # libsb_stt, libsb_mt, libsb_tts_magpie, libsb_tts_vits
└── models/                   # fetched by scripts/fetch-models.sh (or a mounted volume)
    ├── stt/…gguf
    ├── mt/…gguf
    ├── tts_magpie/…gguf
    └── tts_vits/vits-mms-*/
```

`sb-server` needs `lib/` and `models/` reachable via `SB_LIB_DIR` / `SB_MODELS_DIR`.
The libs are loaded with `dlopen(RTLD_LOCAL)` and resolve their own transitive
deps via `@loader_path` (macOS) / `$ORIGIN` (Linux) — keep them together.

## systemd (Linux)

`deploy/systemd/speechbridge.service` — single unit, `MemoryMax=8G`,
`Restart=on-failure`, hardened (`ProtectSystem=strict`, `NoNewPrivileges`).

```sh
sudo cp -r . /opt/speechbridge
sudo cp deploy/systemd/speechbridge.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now speechbridge
journalctl -u speechbridge -f
```

## Docker (Linux)

`deploy/docker/Dockerfile` — multi-stage, models are **not** baked in; mount
them at `/models`.

```sh
docker build -f deploy/docker/Dockerfile -t speechbridge .
docker run --rm -p 8080:8080 -v "$PWD/models:/models" speechbridge
```

Build on an x86_64 host — every native component builds per-platform, no cross-compilation.

## Limits & lifecycle

| limit | value | behavior when exceeded |
|-------|-------|------------------------|
| request body | `SB_MAX_BODY_MB` (25) | 413 `body_too_large` |
| batch audio | `SB_MAX_AUDIO_SEC` (120) | 413 `audio_too_long` |
| batch queue | `SB_QUEUE_DEPTH` (8) | 429 `queue_full` + `Retry-After: 2` |
| batch request | 180 s | context cancelled |
| WS idle | 60 s | connection closed |
| concurrent WS | `SB_STREAMS_MAX` (4) | 503 on connect |

Graceful shutdown is **not yet implemented** (see `docs/blockers.md` #12) —
SIGINT/SIGTERM terminate the process immediately, the OS default. Safe in
practice (no on-disk state to corrupt), but an in-flight batch request or WS
session gets no chance to finish; systemd's `TimeoutStopSec` would `SIGKILL`
after a grace period regardless. The dlopen'd libraries are never `dlclose`d
either way (they live for the process).

## Observability

- Logs: structured JSON on stdout with `request_id` / `session_id`, `stage`,
  `duration_ms` (`SB_LOG_FORMAT=text` for a human-readable form). Transcript
  and translation text are logged **only at `SB_LOG_LEVEL=debug`** — user
  speech is private.
- Metrics: `GET /metrics` (Prometheus). Scrape it; alert on `sb_core_up == 0`
  and on `sb_stage_duration_seconds` p95.
- Readiness: point your load balancer at `GET /ready` (503 until STT + every
  configured core is up).

## Benchmarking

With the server running and models loaded:

```sh
make bench                     # uses the bundled English fixture, en -> ru
SB_BENCH_WAV=clip.wav SB_BENCH_SRC=ru SB_BENCH_DST=uz make bench
```

Prints per-stage latency and checks it against the budget (STT partial < 300 ms,
EOU → translation < 2 s, EOU → first audio < 4 s, batch 10 s clip < 15 s on
4 CPU cores). CPU-only numbers; Apple-Silicon `--metal` builds are faster.

### Reference numbers

macOS arm64, **CPU-only** (no Metal), 8 GB RAM, all four cores loaded,
7.4 s English fixture clip → Russian:

| metric | measured | budget |
|--------|---------:|-------:|
| batch end-to-end | ~13 s | < 15 s (10 s clip) |
| EOU → translation | ~1.2 s | < 2 s ✓ |
| EOU → first audio | ~4.7 s | < 4 s |
| STT partial lag | ~1.0 s | < 300 ms |
| batch stages | stt 6 s / mt 4 s / tts 3 s | — |

STT partial lag and EOU→audio miss the budget on this CPU-only,
memory-constrained host; the budget assumes Metal for STT+MT on Apple Silicon
(mission: "macOS/Metal STT+MT expected notably faster"). Re-run `make bench`
after `./scripts/build.sh --metal` on a full-Xcode machine and record the
numbers here.

Linux x86_64, **CPU-only** (release build, AVX2+FMA, no `--metal`), 16-core /
31 GB host, all four cores loaded, 7.4 s English fixture clip → Russian:

| metric | measured | budget |
|--------|---------:|-------:|
| batch end-to-end | ~5.9 s | < 11 s (7 s clip) ✓ |
| EOU → translation | ~1.8 s | < 2 s ✓ |
| EOU → first audio | ~2.9 s | < 4 s ✓ |
| STT partial lag | ~0.8 s | < 300 ms |
| batch stages | stt 1.8 s / mt 2.5 s / tts 1.7 s | — |

More CPU cores and RAM than the macOS reference host close most of the gap:
batch end-to-end and EOU→audio both clear budget here (they missed it on
macOS CPU-only). STT partial lag still misses the 300 ms budget — that number
assumes Metal-accelerated STT, which this CPU-only run doesn't have.

