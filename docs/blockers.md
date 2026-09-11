# Blockers & deviations

Anything that could not be solved cleanly within the hard constraints, with the
workaround chosen and why. `third_party/` is never hand-edited.

---

## 1. parakeet.cpp ggml patches — pre-applied during vendoring

**Status:** resolved (workaround), no upstream change.

`third_party/parakeet.cpp` carries four ggml patches in
`third_party/ggml-patches/` and applies them at CMake **configure** time via
`scripts/apply_ggml_patches.sh`, which requires a git repository inside
`third_party/ggml`. The vendoring procedure deletes every inner `.git`, so that
script can no longer run.

Two independent reasons it does not run from our build anyway:

- No `.git` in the vendored `third_party/ggml` tree.
- parakeet's `CMakeLists.txt:48` guards the call with
  `EXISTS "${CMAKE_SOURCE_DIR}/scripts/apply_ggml_patches.sh"`.
  `${CMAKE_SOURCE_DIR}` is **our** top-level project when the tree is vendored
  as a subdirectory, so the guard is false and the step is skipped silently.

**Workaround:** the four patches were applied to
`third_party/parakeet.cpp/third_party/ggml/` once, during vendoring, with
`git apply` (verified each now reverse-applies cleanly). The patch files are
kept verbatim under `third_party/ggml-patches/` for audit. Net effect is
identical to what parakeet's own build system would have produced. Recorded in
`third_party/PINNED.md`.

Patches: `0001` broadcast-fold in llamafile sgemm (CPU perf); `0002` Metal
conv-2d-dw; `0003` Metal pad-leading; `0004` CUDA pad grid-stride.

---

## 2. parakeet.cpp `${CMAKE_SOURCE_DIR}` include path — fixed in wrapper CMake

**Status:** resolved (workaround), no upstream change.

`third_party/parakeet.cpp/CMakeLists.txt:107`:

```cmake
target_include_directories(parakeet PUBLIC include PRIVATE src ${CMAKE_SOURCE_DIR}/third_party)
```

`${CMAKE_SOURCE_DIR}` resolves to our project root when parakeet is
`add_subdirectory()`'d, so `dr_wav.h` (in parakeet's own `third_party/`) is not
found and the engine fails to compile.

**Workaround:** `cores/stt/CMakeLists.txt` appends the correct path after
`add_subdirectory`:

```cmake
target_include_directories(parakeet PRIVATE "${SB_ENGINE_DIR}/third_party")
```

`target_include_directories` on an existing target from the parent scope is
supported and does not modify the vendored files.

---

## 3. ggml version string shows the wrong commit — cosmetic

**Status:** accepted, cosmetic only.

With the inner `.git` removed, ggml's CMake version detection walks up and finds
the Speech Bridge repo's `.git`, so configure prints e.g.
`ggml commit: <speechbridge short sha>`. This only affects a logged version
string; the pinned source is recorded in `third_party/PINNED.md`.

---

## 4. Metal backend gated OFF by default

**Status:** partial — CPU-first path complete; Metal wired but unverified here.

The mission wants Metal on Apple Silicon (parakeet's Metal ggml patches;
`GGML_METAL_EMBED_LIBRARY`; llama.cpp `-DGGML_METAL=ON`). The development
machine has Xcode **Command Line Tools only** — no `metal` / `metallib` shader
compiler — so a Metal build cannot be produced or verified here.

**Workaround:** every ggml core's `CMakeLists.txt` has `option(SB_METAL ... OFF)`
and `scripts/build.sh --metal`; when enabled on a host with full Xcode it
forwards `GGML_METAL=ON` + `GGML_METAL_EMBED_LIBRARY=ON` (and
`PARAKEET_GGML_METAL=ON` for STT). `doctor.sh` reports whether the Metal
toolchain is present. Default builds are CPU-first (mission: "CPU-first; Metal
on Apple Silicon"). All functional verification here (STT transcript, TTS audio,
batch, streaming) was done CPU-only; Metal remains to be verified on a
full-Xcode machine.

---

## 5. STT model — no public Uzbek fine-tune

**Status:** accepted, using the base multilingual model.

The mission names the STT model as a "nemotron uz-streaming fine-tune
(uz, ru, kaa)". No such checkpoint is published as GGUF (or at all, publicly).

**Chosen:** `nemotron-3.5-asr-streaming-0.6b` (GGUF from
`mudler/parakeet-cpp-gguf`) — NVIDIA's prompt-conditioned, cache-aware streaming
ASR across 40+ locales including uz and ru, with EOU. Karakalpak (kaa) coverage
is unverified. Recorded in `models/MANIFEST.md`. Drop-in replaceable if a real
uz fine-tune appears — only the manifest entry and `SB_STT_MODEL` change.

---

## 6. Uzbek & Karakalpak VITS TTS voices — need offline ONNX export

**Status:** resolved. Both voices exported and verified producing real audio
via `/v1/speak` (uz: 0.42s/13KB, kaa: 0.26s/8KB WAV, non-silent — peak/RMS
checked). `/v1/capabilities` now reports `"vits":["kaa","ru","uz"]`.

`libsb_tts_vits` (sherpa-onnx / MMS-TTS) is meant to cover uz, ru, kaa — the
languages magpie lacks. sherpa-onnx publishes a pre-built package only for
Russian (`vits-mms-rus`, in the `tts-models` GitHub release). The upstream
weights for Uzbek (`facebook/mms-tts-uzb-script_cyrillic`) and Karakalpak
(`facebook/mms-tts-kaa`) exist but only in HF-Transformers format.

**Correction to the originally planned recipe:** sherpa-onnx's current repo
does **not** contain a `scripts/mms/export-onnx-mms.py` — that script does not
exist upstream (checked at export time). The actual working recipe traces the
HF-Transformers VITS checkpoint to ONNX directly with `torch.onnx.export`,
using the export script and `monotonic_align` Cython extension from the
`mms-meta/MMS` HF Space (the same one `csukuangfj/vits-mms-rus` — the
pre-built Russian voice sherpa-onnx already ships — was itself built with).

**Recipe actually used** (offline, one-time, outside build/runtime — does not
violate "no Python anywhere in build or runtime"):

1. `python3.11 -m venv venv` (needs `python3.11-dev` headers for the Cython
   build below; the system default `python3` at the time was 3.12 without
   headers installed). Install: `onnx scipy Cython numpy onnxscript` plus
   `torch` from `https://download.pytorch.org/whl/cpu` (the recipe's original
   `torch==1.13.0+cpu` pin is no longer published; latest CPU wheel — 2.14.0 at
   export time — works with the fix in step 4).
2. `git clone --depth 1 https://huggingface.co/spaces/mms-meta/MMS` — build its
   `vits/monotonic_align` Cython extension in place (`setup.py build_ext
   --inplace` from within `vits/monotonic_align/`, copy the built `.so` next to
   `core.pyx`, `sed 's/\.monotonic_align\.core/.core/g'` on
   `vits/monotonic_align/__init__.py` since the extension is built flat, not as
   a subpackage).
3. Fetch checkpoint + config + vocab for each language from
   `https://huggingface.co/facebook/mms-tts/resolve/main/models/<lang>/`
   (`G_100000.pth`, `config.json`, `vocab.txt`) — language codes `uzb`
   (Cyrillic script) and `kaa`.
4. Run the export script from
   `https://huggingface.co/csukuangfj/vits-mms-rus/raw/main/vits-mms.py` with
   `PYTHONPATH` including the cloned `MMS` and `MMS/vits` dirs and
   `language=<lang>` set, **with one required patch**: add `dynamo=False` to
   the `torch.onnx.export(...)` call. Without it, torch ≥2.x defaults to its
   new dynamo/`torch.export`-based tracer, which fails with
   `GuardOnDataDependentSymNode` on `vits/transforms.py:105`
   (`if torch.min(inputs) < left or torch.max(inputs) > right:` — VITS's
   rational-quadratic-spline flow has genuinely data-dependent control flow
   that the strict symbolic tracer rejects). `dynamo=False` forces the legacy
   TorchScript-based tracer this script was originally written against, which
   handles it fine (with only `TracerWarning`s, not errors).
5. Produces `model.onnx` + `tokens.txt`; copy to
   `$SB_MODELS_DIR/tts_vits/vits-mms-{uzb,kaa}/`.

**Why these aren't in `fetch-models.sh`'s manifest block:** that script only
downloads from fixed, stable URLs it can sha256-verify. These two files have no
such stable public URL — they're a local export, not a published release
artifact (unlike `vits-mms-rus`, which csukuangfj published to HF). They are
gitignored like all other model weights; re-run the recipe above to reproduce
them. sha256 recorded in `models/MANIFEST.md` for provenance of the exact
files verified working, not for `fetch-models.sh` to check.

---

## 7. MADLAD-400 GGUFs vs current llama.cpp T5 loader

**Status:** resolved — switched to a standard-schema GGUF.

The first MT model tried, `cstr/madlad400-3b-mt-GGUF` (2024), was produced with
a **custom converter** using pre-standardization metadata keys (`t5.d_model`,
`t5.n_heads`, `t5.encoder.n_layers`, …). The vendored 2026 llama.cpp T5 loader
reads the standardized names (`t5.embedding_length`, `t5.attention.head_count`,
`t5.block_count`, `t5.context_length`, …) and rejects the file with
`key not found in model: t5.context_length` (then `t5.embedding_length`, …).

**Resolution:** `models/MANIFEST.md` now points at
`mtsdurica/madlad400-3b-mt-Q4_K_M-GGUF`, converted with the **standard**
llama.cpp converter — it carries the modern key schema and loads cleanly.

`app/cmd/gguf-kv` remains as a small GGUF metadata inspector/patcher
(`get`, `ensure-u32`) for future one-off fixes; it is not needed for the
default model set.

---

## 8. tts_vits deployment target 12.0 vs prebuilt onnxruntime (13.4)

**Status:** accepted (link-time warning only).

sherpa-onnx fetches a prebuilt static `onnxruntime` for `osx-arm64` built for
macOS 13.4. Linking it into `libsb_tts_vits.dylib` at deployment target 12.0
prints `ld: warning: object file … was built for newer 'macOS' version (13.4)`.
The lib runs fine on the build host. To ship to macOS 12 the vits core would
need `CMAKE_OSX_DEPLOYMENT_TARGET=13.4` (the other three cores stay at 12.0).

---

## 9. Go toolchain requirement is 1.23, not 1.22

**Status:** accepted (minor).

The mission asks for Go ≥ 1.22. `github.com/coder/websocket` (the WebSocket
library for `/v1/stream` — a single, zero-dependency module) declares
`go 1.23`, which propagates to `go.mod`. 1.23 (Aug 2024) is the effective
minimum. `doctor.sh` checks for it. Hand-rolling RFC 6455 to shave one minor
version was judged not worth the framing/masking-bug risk.

---

## 10. Browser WebSocket clients cannot authenticate `/v1/stream` via header

**Status:** resolved — query-param fallback added, milestone 9 (Vue frontend).

`/v1/stream` was gated by the same `Authorization: Bearer` middleware as the
batch REST routes. The `Authorization` header cannot be set on a browser
`WebSocket` handshake (the API gives no hook for custom headers), so with
`SB_AUTH_TOKEN` set, the shipped Vue frontend's live session had no way to
authenticate the stream at all.

**Resolution:** `app/internal/httpapi/server.go` adds `authWS`, used only for
`GET /v1/stream`: it checks the `Authorization` header first (unchanged for
non-browser clients, e.g. `clients/go`) and falls back to an `access_token`
query parameter. Every other `/v1/*` route is untouched — still header-only.
Documented in `docs/api.md`. The frontend's WS client
(`web/frontend/src/api/stream.ts`) sends the token this way.

Also corrected `docs/api.md`: `GET /v1/capabilities` was documented as
requiring auth but has never actually enforced it in `server.go` (it's grouped
with `/health`/`/ready`/`/metrics` as always-open, deliberately, so a
not-yet-authenticated UI can populate its setup screen). Docs now match code.

---

## 11. `libsb_tts_vits.so` crashed loading any VITS model on Linux x86_64

**Status:** resolved.

`sb_tts_model_load()` (and therefore `/ready`, since all cores load eagerly at
startup) crashed the whole process on this platform: `free(): invalid
pointer` / `SIGABRT`, raised from inside a `std::regex` construction, called
from `onnxruntime::DeviceDiscovery::DiscoverDevicesForPlatform()`, called
unconditionally from `Environment::Initialize()` the first time any
`OrtEnv` is created (`SherpaOnnxCreateOfflineTts` → `OrtApis::CreateEnv`).
Reproduced standalone with a 20-line `dlopen` harness under `gdb` — the crash
is entirely inside the vendored/prebuilt `onnxruntime` static archive, before
any of our wrapper code (`cores/tts_vits/sb_tts_vits.cpp`) runs, so nothing at
that layer could catch or route around it.

`DeviceDiscovery` is a relatively new (~onnxruntime 1.20+) automatic
execution-provider/hardware enumeration feature. It has multiple open upstream
reports of the same crash signature on other platforms (ARM64 Jetson —
microsoft/onnxruntime#28301 — and RK3588/simple-framebuffer VMs —
microsoft/onnxruntime#26763) that a maintainer PR (#28344) partially
addressed; our case reproduces on a plain Intel x86_64 host with a normal
`vendor_id`, so it isn't just the "unknown CPU vendor" variant those cover.

Two things were tried:

1. sherpa-onnx's vendored ggml-adjacent `SHERPA_ONNX_LINK_LIBSTDCPP_STATICALLY`
   defaults `ON` on Linux, statically linking a second libstdc++ into our
   otherwise-dynamically-linked `sb_tts_vits.so` — a real (if different)
   footgun. Turned off via that upstream CMake option in
   `cores/tts_vits/CMakeLists.txt` (no vendored source touched). Verified this
   alone does **not** fix the crash — kept anyway since it removes a
   plausible second source of heap-allocator confusion and matches how every
   other core links libstdc++.
2. **The actual fix:** `third_party/sherpa-onnx/cmake/onnxruntime-linux-x86_64-static.cmake`
   hardcodes onnxruntime 1.27.1 (URL + sha256, both literal). Since that file
   can't be edited, `cores/tts_vits/CMakeLists.txt` now fetches onnxruntime
   **1.18.1** itself (sha256-verified, same discipline as
   `scripts/fetch-models.sh`) — confirmed by symbol inspection to predate
   `DeviceDiscovery` entirely — into the build tree, and points CMake's own
   `FETCHCONTENT_SOURCE_DIR_ONNXRUNTIME` override at it. That variable is
   FetchContent's built-in mechanism for a parent project to replace a named
   dependency's source wholesale; when set, the vendored
   `FetchContent_Declare(onnxruntime URL ... HASH ...)` call's own URL/hash is
   never consulted for that dependency. Gated to
   `CMAKE_SYSTEM_NAME STREQUAL Linux AND CMAKE_SYSTEM_PROCESSOR STREQUAL
   x86_64` so it cannot affect the macOS arm64 build (which uses a different
   sherpa-onnx cmake file for its own onnxruntime fetch, verified working in
   milestone 8).

Verified: the standalone `dlopen` repro now loads real VITS voice
directories successfully; `make test-e2e` passes end-to-end
(`POST /v1/speech-to-speech`, `en → ru`, produces real synthesized audio);
`/ready` reports `tts_vits: "ok"`.

If sherpa-onnx's vendored pin ever moves past a fixed onnxruntime release,
re-check whether this override is still needed — drop it if so, rather than
carrying a stale downgrade forward.

---

## 12. Orchestrator rewritten Go → C++ (`pure-cpp` branch)

**Status:** done. `app/` (Go), `clients/go/`, `go.mod`/`go.sum` removed. The
four core libs and vendored engines are byte-for-byte unchanged — only the
process that dlopens them changed language. Same wire contract throughout;
`docs/api.md` never changed.

New dependencies (`third_party/PINNED.md`): uWebSockets + uSockets (HTTP+WS),
nlohmann/json, Catch2 (unit tests). `server/src/native/shim.c`/`shim.h` are a
straight copy of the Go build's cgo shim — pure C already, it needed no
porting, just an `extern "C"` fix (see below).

Verified: identical transcript/translation output to the prior Go server on
the same fixture, in both `POST /v1/speech-to-speech` and `/v1/stream`
(real models, not fakes). `make test` (109 assertions, ported 1:1 from the Go
suite) green.

Four real bugs surfaced getting the C++ version to parity, each worth
recording since they're easy to reintroduce:

1. **`shim.h` had no `extern "C"`.** It was previously only ever included
   from `shim.c` (compiled as C); a C++ translation unit mangles its bare
   declarations, so `native_core.cpp` failed to link against `shim.c`'s C
   symbols despite them existing in the same archive. Fixed by wrapping
   `shim.h`'s own declarations in `#ifdef __cplusplus extern "C" { ... }
   #endif` (the included ABI headers already had this; the shim's own
   wrapper functions didn't).
2. **`uWS::Loop::get()` is thread-local.** Session's MT→TTS work runs on a
   detached worker thread (see `ARCHITECTURE.md`); calling `Loop::get()`
   from that thread doesn't return the server's real event loop — it lazily
   creates a brand-new, never-run loop local to that thread, so every
   `defer()` onto it silently vanishes (translation/audio/done messages
   never sent, no error). Fixed by capturing the real loop pointer once in
   the WS `upgrade` handler (which does run on the actual server loop
   thread) and threading it through explicitly instead of ever calling
   `Loop::get()` off that thread.
3. **uWS commits the response status line on the first header/body byte
   written.** A `writeStatus()` call after that point is silently ignored —
   the client sees "200 OK" on the wire even though the JSON body says
   `{"error":...}`. This bit every batch handler, which wrote an eager
   `X-Request-ID` header before knowing the final status. Fixed by
   centralizing every response through `WriteJSON`/`WriteError` helpers
   (`server/src/httpapi/errors.h`) that always call `writeStatus()` first;
   no handler may call `res->writeHeader()` before the outcome is known.
4. **Closing the WS connection right after the (now-async) `Stop()`
   returned dropped every pending translation/audio/`done` message** — the
   non-blocking redesign (see `ARCHITECTURE.md`'s stream.cpp note) means
   `Stop()` can return well before in-flight sentence workers finish. The
   close now happens only inside the `send` callback, once a `done` message
   is actually transmitted, mirroring the Go version's
   blocking-`Stop()`-then-`Close()` ordering without blocking the shared
   event loop.

Also fixed in passing: `obs::Logger` wrote to `std::cout` without an
explicit flush per line, which under a redirected/piped stdout (i.e. always,
outside an interactive terminal) can sit in the buffer indefinitely — a
crash before the next flush would silently lose log lines. Switched to
flushing every line (`std::endl`).

**Known gaps, carried forward deliberately rather than adding unbounded
scope:**

- **Graceful shutdown is not implemented.** The Go version drained
  in-flight requests for up to 30s on SIGINT/SIGTERM before exiting; the
  C++ `main.cpp` has no signal handling at all, so the OS's default
  behavior (immediate termination) applies. Safe in practice (no on-disk
  state to corrupt; systemd's `TimeoutStopSec` would `SIGKILL` after a
  grace period regardless), but a dropped WS session or in-flight batch
  request gets no chance to finish. Revisit if this matters for a given
  deployment.
- **`gguf-kv`** (the Go GGUF metadata inspector/patcher from blocker #7)
  was not ported — it was already "not needed for the default model set,"
  kept only for future one-off fixes. Re-implement in C++ (or reach for
  `llama-gguf`-style tooling from `third_party/llama.cpp` directly) if a
  future model swap needs metadata surgery.
- **`sb-bench` was ported to Node** (`scripts/bench.mjs`, invoked by
  `scripts/bench.sh`) rather than C++ — it's a development-only latency
  probe against the running server's own HTTP/WS API, not part of the
  shipped artifact, and Node was already a build-time dependency for the
  frontend. Reuses the WS testing pattern validated against the real
  server during this rewrite.
- **No C++ equivalent of the Go pipeline's in-process fake-engine unit
  tests** (`core.FakeSTT`/`FakeMT`/`FakeTTS`, used to test batch
  degraded-mode branching and httpapi handlers without models). `make
  test-e2e` (`scripts/test-e2e.sh`) covers the same ground black-box, over
  the real HTTP API with real models, but only when models are present.
  Porting the fakes would let that logic run under plain `make test` too —
  worth doing if `pipeline.cpp`'s branching grows more complex.

---

## 13. MT and VITS TTS were both silently under-threaded

**Status:** resolved (MT, VITS); STT and magpie confirmed not affected /
not fixable from our layer.

Investigated while lowering the live-session latency budget. Two of the
four cores were leaving most of a many-core host's CPU idle:

- **MT (`cores/mt/sb_mt.cpp`).** `llama_context_params.n_threads = 0` reads
  like "let llama.cpp pick a sensible default." It doesn't: tracing through
  `third_party/llama.cpp/ggml/src/ggml-cpu/ggml-cpu.c`'s `ggml_graph_plan()`,
  `n_threads <= 0` resolves to the **compile-time constant**
  `GGML_DEFAULT_N_THREADS = 4` — regardless of the host's actual core count.
  On the 16-core box this was verified on, MT inference was running on 4
  threads the whole time.
- **VITS (`cores/tts_vits/sb_tts_vits.cpp`).** `cfg.model.num_threads` was
  hardcoded to `2`, no host-awareness at all.

Fix: both now compute a real thread count from
`std::thread::hardware_concurrency()`, capped (8 for MT, 4 for VITS) rather
than used raw — sentences translate/synthesize concurrently on separate
worker threads (`server/src/httpapi/session.cpp` dispatches one per
sentence, and up to `SB_STREAMS_MAX` WS sessions run at once), so an
uncapped per-call thread count would oversubscribe the CPU exactly when
latency matters most. Chosen empirically: bench numbers matched expectation
(substantial, reproducible win) at these caps; not exhaustively tuned
further. Measured before/after on the same host/clip in
`docs/operations.md`'s benchmarking section — batch end-to-end -26%, MT
-25%, TTS -41%, EOU→translation -30%, EOU→first audio -31%.

**Confirmed NOT similarly fixable:**

- **STT (`cores/stt/sb_stt.cpp`)** — `parakeet.cpp`'s public API
  (`third_party/parakeet.cpp/include/{parakeet,parakeet_capi}.h`) exposes no
  thread-count parameter at all; whatever it uses internally isn't something
  our wrapper can override without patching the vendored engine. The
  observed "STT partial lag" budget miss (~1s vs. a <300ms target that
  assumes Metal) is therefore likely dominated by the model's own internal
  streaming chunk/cache window, not a thread-starvation bug — nothing to fix
  from `cores/stt/` as it stands.
- **magpie TTS (`cores/tts_magpie/`)** — its flat C API
  (`magpie_tts_capi.h`) also exposes no thread-count parameter, but its
  richer struct-based API defaults `n_threads` to "0 = hardware
  concurrency" (`third_party/magpie-tts.cpp/include/magpie_tts.h`), so the
  flat API is presumably already using it — no fix needed unless proven
  otherwise by a future benchmark.
