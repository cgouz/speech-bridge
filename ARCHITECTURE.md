# Architecture

## One process, in-process pipeline

`sb-server` is a single Go binary that `dlopen`s four C++ core libraries and
runs the whole pipeline in-process. There are **no HTTP calls between pipeline
stages**.

```
mic / WAV ──► STT (parakeet) ──► sentence split ──► MT (MADLAD) ──► TTS ──► PCM out
              streaming            (Go, pure)         per sentence     magpie | vits
              partial + EOU                                           by target lang
```

The **sentence** is the unit of the pipeline: MT context is 512 tokens, TTS
caps ~25 s. Transcripts are split into sentences before MT; TTS runs per
sentence. Batch mode uses the identical per-sentence pipeline, fed from a file.

## ggml isolation (non-negotiable)

parakeet.cpp, llama.cpp and magpie-tts.cpp each bundle their own, mutually
incompatible ggml (different patches, different `GGML_MAX_NAME`). Sharing one
link unit causes silent corruption (truncated tensor names, wrong symbol wins).

Enforcement:

- Each core is built as a **separate CMake project** (`cores/<name>/`), into its
  own build tree, producing one self-contained shared library that statically
  links its own engine + its own ggml.
- All core code compiled `-fvisibility=hidden`; only `SB_API`-marked symbols
  exported. Linux also links `-Wl,-Bsymbolic -Wl,--exclude-libs,ALL` with a
  `sb_*`-only version script; macOS uses an `-exported_symbols_list` of `_sb_*`
  plus two-level namespace.
- `scripts/check-symbols.sh` fails the build on any exported symbol not matching
  `^_?sb_` — a leaked `ggml_*` is a hard failure.
- Go loads each lib with `dlopen(RTLD_NOW | RTLD_LOCAL)` via cgo. Cores are
  never `-l`-linked at Go build time; never `RTLD_GLOBAL`.
- The milestone-2 one-process smoke test loads all four in one process and calls
  each core's `sb_*_abi_version()` — proof of no collision on both platforms.

## Core ABI shape

Common conventions in `cores/common/sb_abi.h`. Every core separates:

- `*_model_load()` — heavy, once per process; weights shared read-only.
- `*_ctx_new()` / `*_stream_new()` — cheap, per session; holds KV-cache /
  encoder / decoder state.

One inference at a time per context — the Go layer serializes with a per-context
gate. A returned C buffer is valid only until the next call on that context and
is copied out immediately on the Go side.

## Repository & module

One Go module rooted at the repo (`github.com/cgouz/speech-bridge`): `app/` is
the server, `web/` the embedded UI, `clients/go/` the SDK. `third_party/` holds
the vendored engines and is excluded from Go builds (`go` commands target
`./app/... ./web/... ./clients/...`).

## Go application

- `internal/core` — cgo `dlopen` loaders, one file per core, typed C shims
  (cgo cannot call function pointers directly), `.so`/`.dylib` chosen at runtime.
- `internal/session` — per-connection state (STT stream, langs, voice).
- `internal/stream` — WebSocket `/v1/stream`.
- `internal/pipeline` — per-sentence STT→MT→TTS with stage overlap (while
  sentence N is in TTS, N+1 is in MT, N+2 is transcribing).
- `internal/text` — sentence splitter + uz Latin↔Cyrillic transliteration,
  pure Go, zero cgo.
- `internal/httpapi` — batch REST, `/health` `/ready` `/metrics`
  `/v1/capabilities`, embedded web UI.
- `internal/config` — `SB_*` env vars, validated at startup (process dies on
  invalid values, never at request time).
- `internal/obs` — slog JSON logs with `request_id`, Prometheus metrics.
  Transcript/translation text logged only at debug level (user speech is
  private).

## TTS engine routing

The pipeline picks the TTS engine by target language: magpie for
{en, de, es, fr, it, pt-BR, hi, ko, vi, ar}, vits for {uz, ru, kaa}.
`/v1/capabilities` reports supported languages per engine, computed at startup
from `sb_tts_languages` and which models actually loaded. MADLAD emits Uzbek in
Cyrillic; `internal/text` transliterates for display and feeds the MMS uz voice
whichever script it expects (documented in `docs/models.md`).
