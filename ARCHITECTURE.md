# Architecture

## One process, in-process pipeline

`sb-server` is a single C++ binary that `dlopen`s four core libraries and
runs the whole pipeline in-process. There are **no HTTP calls between pipeline
stages**.

```
mic / WAV ──► STT (parakeet) ──► sentence split ──► MT (MADLAD) ──► TTS ──► PCM out
              streaming            (C++, pure)        per sentence     magpie | vits
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
- The server loads each lib with `dlopen(RTLD_NOW | RTLD_LOCAL)` (see
  `server/src/native/shim.c`). Cores are never `-l`-linked at server build
  time; never `RTLD_GLOBAL`.
- The one-process smoke test (`cores/smoke/`) loads all four in one process
  and calls each core's `sb_*_abi_version()` — proof of no collision on both
  platforms.

## Core ABI shape

Common conventions in `cores/common/sb_abi.h`. Every core separates:

- `*_model_load()` — heavy, once per process; weights shared read-only.
- `*_ctx_new()` / `*_stream_new()` — cheap, per session; holds KV-cache /
  encoder / decoder state.

One inference at a time per context — the server layer serializes with a
per-context gate (a `std::mutex` per context). A returned C buffer is valid
only until the next call on that context and is copied out immediately.

## Repository layout

`cores/` are the four isolated engine cores (unchanged by the C++ rewrite —
see below). `server/` is the C++ orchestrator. `web/` is the Vue frontend,
served straight off disk by the server (`web/dist/`, build-time-only Node
dependency). `third_party/` holds every vendored dependency — the four
inference engines plus the server's own (uWebSockets, nlohmann/json, Catch2)
— and is never hand-edited; see `third_party/PINNED.md`.

## C++ server (`server/`)

Originally a Go binary; rewritten in C++ (same wire contract throughout —
`docs/api.md` never changed) on top of uWebSockets/uSockets for HTTP+WS,
nlohmann/json for (de)serialization, and Catch2 for unit tests. Layout
mirrors the packages it replaced:

- `src/native/` — `dlopen` loaders (`shim.c`, unchanged pure C — it needed no
  porting at all) wrapped in `core::STT`/`MT`/`TTS` C++ interfaces
  (`core.h`, `native_core.cpp`). `.so`/`.dylib` chosen at compile time.
- `src/httpapi/session.{h,cpp}` — per-connection state (STT stream, langs,
  voice) for one live speaker.
- `src/httpapi/stream.{h,cpp}` — WebSocket `/v1/stream`, adapting uWS frames
  to a `Session`. uWS runs a single-threaded event loop per process; MT→TTS
  work for each sentence runs on a detached worker thread (keeping the
  `Session` alive via `shared_from_this` so it outlives an early client
  disconnect) so STT keeps consuming audio while translation/synthesis run in
  the background — completion is signaled back to the loop thread via
  `uWS::Loop::defer()`, never by blocking it.
- `src/pipeline/` — per-sentence STT→MT→TTS with stage overlap (while
  sentence N is in TTS, N+1 is in MT, N+2 is transcribing).
- `src/text/` — sentence splitter + uz Latin↔Cyrillic transliteration, plus a
  small purpose-built UTF-8/Cyrillic-aware `unicode_lite` module (the C++
  standard library has no Unicode-aware case/classification support).
- `src/httpapi/{server,handlers}.cpp` — batch REST, `/health` `/ready`
  `/metrics` `/v1/capabilities`, the static frontend.
- `src/httpapi/wav.{h,cpp}` — RIFF/WAVE decode + PCM16 encode.
- `src/httpapi/multipart.{h,cpp}` — `multipart/form-data` parsing, built on
  uWS's bundled parser.
- `src/config.{h,cpp}` — `SB_*` env vars, validated at startup (process dies
  on invalid values, never at request time).
- `src/obs/` — structured JSON/text logs with `request_id`/`session_id`, a
  dependency-free Prometheus registry. Transcript/translation text logged
  only at debug level (user speech is private).

`tests/` (Catch2, `sb_tests` target, wired to `make test`) exercises `text`,
`config`, and `wav` with no models and no native libs — the same contract the
former Go unit tests had.

## TTS engine routing

The pipeline picks the TTS engine by target language: magpie for
{en, de, es, fr, it, pt-BR, hi, ko, vi, ar}, vits for {uz, ru, kaa}.
`/v1/capabilities` reports supported languages per engine, computed at startup
from `sb_tts_languages` and which models actually loaded. MADLAD emits Uzbek in
Cyrillic; `server/src/text` transliterates for display and feeds the MMS uz
voice whichever script it expects (documented in `docs/models.md`).
