# Speech Bridge

Production-ready, fully local, real-time **speech-to-speech translation** in
**pure C/C++** (plus a TypeScript/Vue frontend, build-time only). Live meeting
interpretation (speaker in Russian → listener hears an Uzbek voice + live
captions) and batch mode (WAV → transcript, translation, synthesized audio).

Everything runs locally: no cloud APIs, no Python at build or runtime, no
PyTorch, no CUDA toolkit. CPU-first; Metal on Apple Silicon.

**Platforms (both first-class):** Linux x86_64, macOS arm64. Every native
build (cores + server) is CMake, built natively on each.

## How it works

```
mic / WAV ─► STT (parakeet) ─► sentence split ─► MT (MADLAD) ─► TTS ─► PCM out
             streaming           (pure C++)       per sentence    magpie │ vits
             partial + EOU                                        by target lang
```

One process `dlopen`s four self-contained shared libraries and runs the whole
pipeline in-memory — no HTTP between stages. Each core statically links its own
engine **and its own ggml** (the three ggml copies are mutually incompatible
and must never share a link unit). The orchestrator itself (HTTP + WebSocket
server, pipeline, everything under `server/`) is C++ on top of uWebSockets;
see `ARCHITECTURE.md`, `third_party/PINNED.md`.

| core               | engine        | role | script |
|--------------------|---------------|------|--------|
| `libsb_stt`        | parakeet.cpp  | streaming STT (uz/ru/kaa), end-of-utterance events | — |
| `libsb_mt`         | llama.cpp     | MT — MADLAD-400 3B (T5), `<2xx> text`, 512-token ctx | — |
| `libsb_tts_magpie` | magpie-tts.cpp| TTS for en/de/es/fr/it/pt-BR/hi/ko/vi/ar | Latin |
| `libsb_tts_vits`   | sherpa-onnx   | TTS (MMS/VITS) for uz/ru/kaa | Cyrillic |

## Build

Prereqs: CMake ≥ 3.20, a C++17 compiler, GNU Make, bash. Node.js is a
**build-time-only** dependency for the frontend (not needed at runtime — see
below). Run `make doctor` to check the environment.

### Linux x86_64
```sh
sudo apt install build-essential cmake zlib1g-dev             # gcc ≥ 12
make doctor
./scripts/build.sh                # cores + symbol check + smoke + frontend + server
#   -> lib/libsb_*.so   (release builds pin AVX2+FMA, no -march=native)
#   -> sb-server
```

### macOS arm64
```sh
xcode-select --install
brew install cmake
make doctor
./scripts/build.sh                                    # CPU-first
./scripts/build.sh --metal                            # + Metal ggml backend (needs full Xcode)
#   -> lib/libsb_*.dylib
#   -> sb-server
```

### Models (~3.5 GB, architecture-independent)
```sh
make fetch-models                                     # sha256-verified against models/MANIFEST.md
```
Uzbek/Karakalpak VITS voices need a one-off offline export — see
`models/MANIFEST.md` and `docs/blockers.md` #6.

### Frontend (Vue 3 + Vite + TypeScript, build-time only)
```sh
make web                                              # web/frontend -> web/dist/, served from disk
```
Node is a **build-time-only** dependency — the server reads `web/dist/`
straight off disk at startup (`SB_WEB_DIST_DIR`, defaults to `web/dist` next
to the binary) and needs no Node at runtime. `./scripts/build.sh` runs this
automatically when `npm` is on `PATH`; otherwise it skips with a message and
the previously-committed `web/dist/` (tracked in git for exactly this reason)
is used as-is. See `web/frontend/README.md`.

## Run
```sh
make run                                              # SB_BIND defaults to 127.0.0.1:8080
```
- `GET  /`                     Vue frontend (setup, live session, batch panel, observability)
- `GET  /health /ready /metrics /v1/capabilities`
- `POST /v1/speech-to-speech`  multipart WAV → transcript + translation + audio
- `POST /v1/transcribe /v1/translate /v1/speak`
- `WS   /v1/stream`            live speech → ordered partial/transcript/translation/audio

Full contract: `docs/api.md`. Deploy: `docs/operations.md`.

## Test

```sh
make test        # unit tests (Catch2) — no models, no native libs
make check       # build cores → check-symbols → smoke test → build server → unit tests
make test-e2e    # real cores + real models, over the actual HTTP API (skips cleanly if models absent)
make bench       # latency budget against a running server (scripts/bench.mjs, Node)
```

## Status

A from-scratch C++ rewrite of the original Go orchestrator (same wire
contract throughout — `docs/api.md` never changed). Verified end-to-end on
Linux x86_64 against real models, byte-for-byte matching output with the
prior Go implementation on the same fixture:

- all four cores build unchanged; `make check-symbols` shows only `sb_*`
  exports; the one-process `dlopen(RTLD_LOCAL)` smoke test passes.
- `make test` passes with no models/libs (109 assertions, ported 1:1 from the
  former Go test suite); `make check` is green.
- real end-to-end `POST /v1/speech-to-speech`: English clip →
  `"Well, I don't wish to see it any more, observed Phoebe…"` →
  Russian `"Ну, я не хочу больше видеть его, - заметила Фиби…"` → synthesized
  Russian speech. `en→uz` translation renders Latin Uzbek.
- streaming `/v1/stream` emits ordered partial/transcript/translation/audio,
  verified with real audio in both directions.

Known gaps (`docs/blockers.md`): no public Uzbek STT fine-tune → base
multilingual nemotron; **uz/kaa VITS voices deferred** to an offline MMS
export, so the ru→uz *voice* isn't live yet (captions + translation are);
graceful shutdown (draining in-flight requests before exit) is not yet
implemented for the C++ server — SIGINT/SIGTERM terminate immediately.
