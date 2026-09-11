# What Speech Bridge is

Speech Bridge is a **fully local, real-time speech-to-speech translator**. One
person speaks into a microphone in one language; a second person hears a
synthesized voice speaking the translation in another language, with live
captions on screen — the "AI live interpreter" pattern popularized by
Samsung Galaxy S25's on-device call translation, but self-hosted and
open-source. It also has a batch mode: upload a WAV file, get back a
transcript, a translation, and synthesized audio for it.

Nothing leaves the machine it runs on. There is no cloud API call anywhere in
the pipeline — speech recognition, translation, and speech synthesis all run
as native inference on the local CPU (or GPU, if built with `--metal`/`--cuda`).
There is also no Python anywhere in the build or at runtime; the one place
Python *is* used is a one-off, offline step to convert two TTS voice
checkpoints to a runtime format (`docs/blockers.md` #6), not something the
shipped binary ever touches.

The whole thing ships as one binary (`sb-server`) plus four small shared
libraries (`lib/libsb_*.so`/`.dylib`) plus a folder of model weights — copy
those to a machine and run it.

## The pipeline

```
mic / WAV ──► STT (parakeet) ──► sentence split ──► MT (MADLAD) ──► TTS ──► speaker / PCM out
              streaming            (C++, pure)        per sentence     magpie │ vits
              partial + <EOU>                                          (by target language)
```

Speech-to-text, translation, and text-to-speech are three independent, swappable
stages glued together by one C++ process. Audio comes in as small ~250ms
chunks (live mic) or a whole file (batch); text flows out as it's produced,
not only once the whole thing is done — see "Real-time, not batch" below.

## What "technology" means here, concretely

| Layer | What it is | Built with |
|---|---|---|
| Speech recognition (STT) | Streaming transcription, with live partial results and end-of-utterance detection | [`parakeet.cpp`](https://github.com/mudler/parakeet.cpp) running NVIDIA's Nemotron 3.5 ASR streaming model (0.6B params, GGUF/quantized) |
| Translation (MT) | Sentence-by-sentence machine translation across 450+ languages | [`llama.cpp`](https://github.com/ggml-org/llama.cpp) running Google's MADLAD-400 (3B, T5 encoder-decoder architecture, GGUF/quantized) |
| Speech synthesis (TTS) — set A | Natural-sounding voices for English, German, Spanish, French, Italian, Brazilian Portuguese, Hindi, Korean, Vietnamese, Arabic | [`magpie-tts.cpp`](https://github.com/mudler/magpie-tts.cpp) running NVIDIA's Magpie TTS Multilingual (357M, GGUF/quantized) |
| Speech synthesis (TTS) — set B | Voices for Uzbek, Russian, Karakalpak | [`sherpa-onnx`](https://github.com/k2-fsa/sherpa-onnx) running Meta's MMS-TTS (VITS architecture, ONNX) |
| Orchestrator / server | HTTP + WebSocket API, the pipeline glue, session/state management | Custom C++17 on [`uWebSockets`](https://github.com/uNetworking/uWebSockets)/uSockets (event loop, HTTP, WS), [`nlohmann/json`](https://github.com/nlohmann/json) (JSON), [`Catch2`](https://github.com/catchorg/Catch2) (unit tests) |
| Numerical backend | The actual tensor math underneath STT/MT/magpie-TTS | [`ggml`](https://github.com/ggml-org/ggml) — CPU by default, optionally Metal (Apple Silicon) or CUDA (NVIDIA), selected at build time |
| Frontend | Setup panel, live session UI (mic capture, live captions, audio playback), batch upload panel, observability view | Vue 3 + TypeScript + Vite, built once at build time into static files the server itself serves — no Node.js needed at runtime |
| Build system | Every native piece (four cores + server) | CMake ≥ 3.20, native per platform (no cross-compilation) |

Everything under `third_party/` is vendored at an exact pinned commit/tag
(see `third_party/PINNED.md`) and never hand-edited — if a vendored engine
needs different behavior, that logic lives in Speech Bridge's own wrapper
code (`cores/*/sb_*.cpp`) instead.

## Why four separate shared libraries, not one binary linking everything

`parakeet.cpp`, `llama.cpp`, and `magpie-tts.cpp` each bundle their **own**
copy of `ggml` — and those copies are mutually incompatible (different
patches, different compile-time constants like `GGML_MAX_NAME`). Linking two
of them into the same program at once causes silent, hard-to-diagnose memory
corruption, not a clean build error.

The fix: each engine is built as its own **self-contained shared library**
(`libsb_stt.so`, `libsb_mt.so`, `libsb_tts_magpie.so`, `libsb_tts_vits.so`),
statically linking its own engine and its own `ggml` inside, with every
symbol hidden except a small, explicitly exported `sb_*` C API. The server
process loads all four with `dlopen(RTLD_NOW | RTLD_LOCAL)` — the `RTLD_LOCAL`
flag is what keeps each library's symbols invisible to the others, so two
incompatible `ggml`s can coexist safely in one process. A dedicated one-process
smoke test (`cores/smoke/`) and a symbol-export check
(`scripts/check-symbols.sh`, which fails the build on any exported symbol that
isn't `sb_*`) both run on every build to catch a regression here immediately.

This is the single most load-bearing architectural decision in the codebase —
almost every other design choice (the shared `sb_abi.h` conventions, the
`dlopen` shim in `server/src/native/`, why `third_party/` is read-only) exists
to support it.

## Real-time, not batch — how live translation actually streams

A naive pipeline would wait for someone to finish an entire sentence (or
utterance), then translate it, then speak it — with the listener hearing
nothing until each full round-trip completes. Speech Bridge instead:

1. **STT streams partial results** as audio arrives (every ~250ms chunk), not
   just at the end. `server/src/httpapi/session.cpp` watches each growing
   partial transcript for a complete sentence (using a sentence-boundary
   splitter, `server/src/text/split.cpp`) and dispatches it to translation
   **as soon as a later partial confirms it's final** — without waiting for
   the model's own end-of-utterance signal, which is tuned for conversational
   turn-taking pauses, not sentence breaks, and can hold back translation for
   a long time on continuous speech.
2. **A time-based fallback** guards against speech that never produces
   sentence-ending punctuation (common for some languages/speaking styles):
   if a chunk of speech sits untranslated for more than ~3 seconds, it gets
   dispatched anyway, bounding worst-case latency to one rolling chunk
   instead of the whole utterance.
3. **Translation and synthesis run on a detached worker thread per sentence**,
   so the next sentence can already be recognized (and even translated) while
   the current one is still being spoken — sentence *N* can be in TTS while
   *N+1* is in MT while *N+2* is still being transcribed. Results are
   reordered back into the original sequence before being sent to the client
   (`Session::MarkReorder`), since worker threads can finish out of order.
4. Every stage streams a distinct message over one WebSocket connection —
   `partial` (live caption, may still change), `transcript` (a sentence is
   final), `translation`, `audio` (a raw PCM chunk to play immediately) — so
   the browser can update captions and start playing audio the moment each
   piece is ready, not once the whole conversation ends.

## The four cores, one at a time

- **`libsb_stt`** (parakeet.cpp / Nemotron 3.5 ASR): streaming speech-to-text
  for Uzbek, Russian, Karakalpak, or auto-detected. Emits partial hypotheses
  as audio streams in, and an end-of-utterance event when the speaker
  genuinely pauses.
- **`libsb_mt`** (llama.cpp / MADLAD-400): translates one sentence at a time.
  MADLAD is a T5 encoder-decoder model prompted with a language tag
  (`<2xx> text`); context is capped at 512 tokens, matching "one sentence per
  call." Covers 450+ languages, since MADLAD is a general-purpose
  multilingual model, not one trained only for this project's target
  languages.
- **`libsb_tts_magpie`** (magpie-tts.cpp / Magpie TTS Multilingual): natural
  speech synthesis for English, German, Spanish, French, Italian, Brazilian
  Portuguese, Hindi, Korean, Vietnamese, and Arabic.
- **`libsb_tts_vits`** (sherpa-onnx / Meta MMS-TTS, VITS architecture): speech
  synthesis for Uzbek, Russian, and Karakalpak — the languages the Magpie
  voice set doesn't cover. MADLAD emits Uzbek in Cyrillic, and the Uzbek MMS
  voice was trained on Cyrillic too, so synthesis uses Cyrillic directly;
  on-screen captions are transliterated to Latin separately
  (`server/src/text`), keeping that conversion off the latency-critical audio
  path.

The pipeline picks magpie or vits automatically based on the target language;
`GET /v1/capabilities` reports exactly which languages are available per
engine at runtime (only what actually loaded, not a hardcoded list).

## The C++ server (`server/`)

A single-threaded-event-loop HTTP + WebSocket server (uWebSockets/uSockets),
with heavy work (translation, synthesis) offloaded to detached worker
threads so the event loop itself never blocks:

- **`src/native/`** — the `dlopen` machinery: `shim.c` resolves each core's C
  functions via `dlsym` into typed function pointers; `native_core.cpp` wraps
  those in C++ interfaces (`STT`, `MT`, `TTS`) the rest of the server codes
  against, so nothing outside this one directory knows or cares that the
  actual inference engines are loaded as plugins at runtime.
- **`src/httpapi/session.{h,cpp}`** — one `Session` per live WebSocket
  connection: owns the STT stream, buffers/dispatches sentences, tracks
  per-connection settings (source/target language, voice).
- **`src/httpapi/stream.{h,cpp}`** — adapts uWebSockets' WS frame events to
  `Session` calls.
- **`src/pipeline/`** — the actual STT → MT → TTS glue, shared by both the
  live WebSocket path and the batch REST endpoint.
- **`src/text/`** — sentence-boundary splitting and Uzbek Latin↔Cyrillic
  transliteration, plus a small hand-written UTF-8/Cyrillic-aware text module
  (the C++ standard library has essentially no Unicode support).
- **`src/httpapi/{server,handlers}.cpp`** — the batch REST endpoints,
  `/health`/`/ready`/`/metrics`/`/v1/capabilities`, and serving the built
  frontend.
- **`src/obs/`** — structured JSON (or plain-text) logs and a small built-in
  Prometheus metrics registry; actual speech/translation text is logged only
  at debug level, since it's user speech.
- **`src/config.{h,cpp}`** — every runtime setting is an `SB_*` environment
  variable, validated once at startup (the process refuses to start on a bad
  value, rather than failing confusingly mid-request later).

## The frontend (`web/frontend/`)

A small Vue 3 + TypeScript + Vite single-page app: a setup panel (language
pair, voice, connection settings), a live-session view (start/stop mic
capture, live captions, streamed audio playback via the Web Audio API), a
batch panel (upload a WAV, see the result), and a capabilities/observability
view. It's a **build-time-only** dependency — `npm run build` produces static
files (`web/dist/`) that the C++ server reads straight off disk at startup;
the shipped binary needs no Node.js or JavaScript runtime at all. The
committed `web/dist/` means the server works even on a host with no Node
installed at all.

## Build once, run anywhere it targets

Every native piece (all four cores plus the server) is built with CMake,
natively, for whichever platform it's running on — there is no
cross-compilation and no interpreted/JIT runtime, so a build produces one
architecture-specific artifact:

- **Linux x86_64** — the default, portable release build targets a common
  instruction-set baseline (AVX2+FMA, not `-march=native`) so one build runs
  on any reasonably modern x86_64 machine.
- **macOS arm64 (Apple Silicon)** — same cores and server, built with Apple's
  toolchain; optionally with the Metal backend for GPU acceleration
  (`--metal`, needs full Xcode).
- **CUDA** — Linux x86_64 with an NVIDIA GPU and the CUDA toolkit installed
  (`--cuda`). Accelerates STT, MT, and the Magpie TTS voices; the VITS voice
  set (Uzbek/Russian/Karakalpak) stays CPU-only regardless of backend — its
  GPU path would need a build configuration incompatible with the
  per-core isolation this project depends on (`docs/blockers.md` #16).

See `README.md` for the exact build commands and `scripts/run-{cpu,metal,cuda}.sh`
for one-command build-and-run per target.

## API surface (summary — full contract in `docs/api.md`)

- `POST /v1/speech-to-speech` — batch mode: upload a WAV, get back JSON with
  the transcript, translation, and (optionally) synthesized audio.
- `POST /v1/transcribe` / `/v1/translate` / `/v1/speak` — the same three
  stages exposed individually.
- `WS /v1/stream` — the live path: send a JSON "start" message (source/target
  language, voice, sample rate), then a stream of raw PCM audio frames;
  receive back an ordered stream of `partial` / `transcript` / `translation`
  / `audio` messages as they're produced.
- `GET /health` / `/ready` / `/metrics` / `/v1/capabilities` — liveness,
  readiness (which cores actually loaded), Prometheus metrics, and which
  languages/voices are available right now.

## The vendored dependencies, one at a time

Everything below lives under `third_party/`, vendored whole at an exact
pinned commit/tag (`third_party/PINNED.md`) and never hand-edited — if
different behavior is needed, that logic lives in Speech Bridge's own
wrapper code instead (`cores/*/sb_*.cpp`). This section goes deeper than the
summary table above: what each project actually is, on its own terms, not
just the one slice of it Speech Bridge happens to use.

### parakeet.cpp — the speech-recognition engine

[`mudler/parakeet.cpp`](https://github.com/mudler/parakeet.cpp), by Ettore Di
Giacinto and Richard Palethorpe (the team behind
[LocalAI](https://github.com/mudler/LocalAI)). MIT licensed.

A from-scratch C++17 port of NVIDIA NeMo's Parakeet family of speech
recognition models, built on `ggml`. NeMo itself is a full PyTorch training
framework; parakeet.cpp reimplements just the *inference* path so nothing
needs Python, PyTorch, or a CUDA toolkit to run a trained model. It covers
every offline Parakeet architecture NVIDIA has published — CTC, RNNT, TDT,
and hybrid TDT-CTC, from 110M up to 1.1B parameters, English and
multilingual — each validated at **WER 0 against NeMo** (byte-identical
transcripts, not just "close enough"), plus per-word timestamps and
confidence scores matching NeMo's own numbers to within `5e-6`. It also
implements NeMo's **cache-aware streaming** mode with end-of-utterance (EOU)
detection, which is the specific capability Speech Bridge depends on for live
transcription.

According to its own published benchmarks it's faster than NeMo's PyTorch
runtime on both CPU and GPU (median ~1.4x on CPU, up to 4.3x on GPU on the
larger models) while producing identical output, and it substantially
outperforms whisper.cpp on the same hardware. Models ship as GGUF (the same
container format llama.cpp uses) with f16/q8_0/K-quant variants trading size
for accuracy — Speech Bridge uses `nemotron-3.5-asr-streaming-0.6b` at `q4_k`
(~700 MB), NVIDIA's multilingual (40+ locale), prompt-conditioned streaming
model, since no dedicated Uzbek fine-tune is published (`docs/blockers.md`
#5). The whole engine is exposed as a flat, exception-free C API
(`parakeet_capi.h`) designed for exactly this kind of `dlopen`/FFI embedding —
`cores/stt/sb_stt.cpp` is a thin wrapper around it.

### llama.cpp — the translation engine

[`ggml-org/llama.cpp`](https://github.com/ggml-org/llama.cpp), originally by
Georgi Gerganov, now a large multi-maintainer project under the ggml
organization. MIT licensed.

The best-known local LLM inference engine that exists — a dependency-free
C/C++ implementation for running large language models on ordinary hardware,
built on top of `ggml`. It's the reason terms like "GGUF" and "quantization"
are now everyday vocabulary for anyone running models locally. It supports
essentially every backend that exists (CUDA, Metal, Vulkan, HIP/ROCm, SYCL,
CANN, and plain CPU with AVX/AVX2/AVX512/NEON), 1.5-bit through 8-bit
quantization, and ships both a CLI and an OpenAI-compatible HTTP server. It
is normally used for autoregressive text generation, but its lower layers
also implement encoder-decoder (T5-family) architectures, which is the part
Speech Bridge actually uses.

Speech Bridge runs Google's **MADLAD-400** (3B parameters, a T5
encoder-decoder translation model covering 450+ languages) through
llama.cpp's T5 support, quantized to `q4_k_m`. Translation happens one
sentence at a time (`cores/mt/sb_mt.cpp`), building the `<2xx> text` prompt
form MADLAD expects internally and running a 512-token context per call.
llama.cpp is also the one core engine that needed an explicit fix for GPU
offload during this project's own development: its `n_gpu_layers` model
parameter defaults to "CPU only" unless a wrapper sets it, unlike parakeet.cpp
and magpie-tts.cpp which auto-detect a GPU device on their own
(`docs/blockers.md` #16).

### magpie-tts.cpp — the multilingual voice engine

[`mudler/magpie-tts.cpp`](https://github.com/mudler/magpie-tts.cpp), also by
Ettore Di Giacinto / LocalAI. MIT licensed.

A from-scratch C++17/`ggml` port of NVIDIA's **Magpie TTS Multilingual
357M**, including its **NanoCodec** neural audio codec — the component that
turns the model's predicted tokens back into an actual waveform. Like
parakeet.cpp, it exists so this model can run without Python/PyTorch/NeMo:
everything (the acoustic model, the codec, the tokenizer, and the
grapheme-to-phoneme dictionaries for five languages) ships as one
self-contained GGUF file. It reimplements NeMo's full tokenization pipeline
in C++, including IPA phoneme dictionaries embedded directly in the model
file, and is numerically parity-gated against the NeMo reference at every
stage (text encoder, decoder, codec) to differences on the order of
`1e-5`–`1e-6`. Its own benchmarks show roughly 60–70x faster synthesis than
the unoptimized NeMo reference pipeline on CPU, mostly from adding a proper
KV cache the reference implementation doesn't use.

It produces 22.05 kHz mono speech in five voices (Aria, Jason, John, Leo,
Sofia) across English, Spanish, German, French, Italian, Brazilian
Portuguese, Hindi, Korean, Vietnamese, and three Arabic variants — this is
exactly the language/voice set `libsb_tts_magpie` exposes in Speech Bridge
(`cores/tts_magpie/sb_tts_magpie.cpp`), quantized to `q4_k` (~540 MB).

### ggml — the tensor library underneath all three

[`ggml-org/ggml`](https://github.com/ggml-org/ggml), originally by Georgi
Gerganov. MIT licensed.

The C tensor library that `llama.cpp`, `whisper.cpp`, `parakeet.cpp`, and
`magpie-tts.cpp` are all built on — it's the foundational piece that made
"run this model on your laptop's CPU" a normal thing to do, before it grew
GPU backends (CUDA, Metal, Vulkan, HIP, SYCL) as well. It defines the tensor
op graph, the memory model, the GGUF file format used to store quantized
weights, and the various integer quantization schemes (`q4_k`, `q5_k`,
`q6_k`, `q8_0`, and others) that make it possible to run a multi-billion
parameter model in a few hundred megabytes of RAM.

Speech Bridge doesn't use `ggml` directly — it's an implementation detail
pulled in transitively by three of the four core engines. But its
architecture directly *shapes* Speech Bridge's own architecture: parakeet.cpp,
llama.cpp, and magpie-tts.cpp each vendor their **own separate copy** of
`ggml`, patched or configured differently (parakeet.cpp carries four of its
own patches on top; magpie-tts.cpp needs `GGML_MAX_NAME=128` instead of the
default 64, since its tensor names run longer). Those three copies are
mutually incompatible in one process — which is the entire reason Speech
Bridge's four cores exist as separate, isolated shared libraries in the first
place (see "Why four separate shared libraries" above). Runtime GPU device
selection (automatically picking Metal or CUDA when compiled in and
available) is also `ggml`'s own `ggml_backend_dev_*` API doing the work,
transparently, underneath parakeet.cpp and magpie-tts.cpp.

### sherpa-onnx — the Uzbek/Russian/Karakalpak voice engine

[`k2-fsa/sherpa-onnx`](https://github.com/k2-fsa/sherpa-onnx), by the k2-fsa
project (the team also behind the `k2` and `icefall` speech projects).
Apache-2.0 licensed.

A very broad, production-oriented speech toolkit built on ONNX Runtime
(rather than `ggml`) — it covers speech recognition, speech synthesis, source
separation, speaker identification/diarization/verification, spoken-language
identification, audio tagging, voice activity detection, keyword spotting,
punctuation restoration, and speech enhancement, with bindings for C++, C,
Python, JavaScript, Java, C#, Kotlin, Swift, Go, Dart, Rust, and Pascal,
running on essentially every OS and CPU architecture that exists (it even
targets WebAssembly and Flutter/Tauri apps). It's a genuinely large,
general-purpose project; Speech Bridge uses one narrow slice of it.

`cores/tts_vits/sb_tts_vits.cpp` links only sherpa-onnx's **offline TTS C
API**, configured for the **VITS** model architecture, to run Meta's
**MMS-TTS** (Massively Multilingual Speech) voices for Uzbek, Russian, and
Karakalpak — the three languages magpie-tts.cpp's voice set doesn't cover.
Every other sherpa-onnx feature (ASR, diarization, the other language
bindings, GPU support) is compiled out via CMake options
(`SHERPA_ONNX_ENABLE_*`), keeping the resulting `libsb_tts_vits.so` to just
what this one code path needs. Unlike the three `ggml`-based cores,
sherpa-onnx's GPU acceleration path needs a different, dynamically-linked
ONNX Runtime build that conflicts with Speech Bridge's static-link
per-core isolation, so this one core stays CPU-only regardless of build
target (`docs/blockers.md` #16) — a real, documented trade-off, not an
oversight.

### uWebSockets / uSockets — the HTTP and WebSocket server

[`uNetworking/uWebSockets`](https://github.com/uNetworking/uWebSockets) (and
its companion socket layer, `uSockets`), by Alex Hultman. Apache-2.0
licensed.

A header-mostly C++17 HTTP and WebSocket server library built for extreme
throughput and low memory use — by its own numbers, fast enough to serve
encrypted TLS traffic quicker than many alternative servers handle even
plain, unencrypted connections. It has held a perfect score on the Autobahn
WebSocket conformance test suite since 2016, runs continuous fuzz testing
under Google's OSS-Fuzz program, and is used in production by several large
cryptocurrency exchanges handling billions of dollars in daily trading
volume — a library chosen for being both fast and battle-tested, not just
fast. It's also the engine underneath Node.js's own popular
`uWebSockets.js` bindings.

It provides Speech Bridge's entire HTTP/WebSocket layer: the single-threaded
event loop, the URL router (`server/src/httpapi/handlers.cpp`'s REST
endpoints), and the WebSocket upgrade/frame handling
`server/src/httpapi/stream.cpp` adapts into `Session` calls for
`/v1/stream`. Speech Bridge vendors it trimmed down to only what it needs —
no TLS (it binds a local/internal address only, matching the project's
original design) and no HTTP/3-over-QUIC — which is also why the server can
run detached worker threads for translation/synthesis without ever blocking
that one event loop thread, deferring completion back to it via
`uWS::Loop::defer()`.

### nlohmann/json — JSON (de)serialization

[`nlohmann/json`](https://github.com/nlohmann/json), by Niels Lohmann. MIT
licensed.

The de facto standard JSON library for modern C++ — a single header that
makes JSON feel like a native C++ type (`json j = {{"key", "value"}};`,
range-based iteration, implicit conversions to/from standard containers)
rather than something requiring a separate parser API. It's one of the most
widely used C++ libraries that exists, vendored here as the project's own
official single-header amalgamation (not a full clone, since that's
literally what the upstream project publishes it for). Every JSON body
Speech Bridge's HTTP API sends or receives — capabilities, error shapes, the
WebSocket protocol's `partial`/`transcript`/`translation`/`audio` messages —
goes through it.

### Catch2 — the unit test framework

[`catchorg/Catch2`](https://github.com/catchorg/Catch2), maintained by the
Catch2 organization (originally Phil Nash). Boost Software License 1.0.

A widely-used, header-only-friendly C++ test framework known for expressive,
low-boilerplate assertions (`REQUIRE(x == y)` reads like the assertion
itself, with no separate "expected vs. actual" argument order to remember)
and BDD-style `SECTION`/`GIVEN`/`WHEN`/`THEN` test organization, without
needing a separate mocking library bolted on for most everyday needs.
Vendored here as its official two-file amalgamated distribution
(`catch_amalgamated.{hpp,cpp}`) — no build system or test suite of Catch2's
own to compile, just the framework itself. It backs `sb_tests`
(`server/tests/`, wired to `make test`), which exercises the sentence
splitter, config validation, and WAV encode/decode with no models and no
native libraries loaded — the same "runs anywhere, instantly" contract the
project's former Go unit test suite had.

## Current status

A from-scratch C++ rewrite of an earlier Go implementation, verified
end-to-end against real models on the same wire contract throughout — see
`README.md`'s Status section and `docs/blockers.md` for the full history of
issues found and fixed along the way (ggml isolation bugs, threading/latency
fixes, the real-time dispatch design, TTS voice gaps, GPU backend wiring).
