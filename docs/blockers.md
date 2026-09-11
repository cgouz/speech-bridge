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

**Status:** resolved. Both voices exported and verified producing correctly
paced, real audio via `/v1/speak` fed genuine Cyrillic text (duration now
scales with text length like the known-good `ru` voice — see #15 for why
earlier duration numbers here were misleadingly short). `/v1/capabilities`
reports `"vits":["kaa","ru","uz"]`.

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
   headers installed). Install: `onnx scipy Cython numpy` plus
   `torch==1.13.0+cpu --index-url https://download.pytorch.org/whl/cpu` — this
   exact pin from the original csukuangfj recipe **is** still available as a
   `cp311` wheel there (an earlier pass here mistakenly concluded it wasn't,
   from a bare `pip install torch==1.13.0+cpu` without the CPU index URL, and
   used the latest wheel — 2.14.0 at the time — plus a `dynamo=False` patch to
   force its legacy tracer instead; that export was never actually the
   problem — see #15 — but 1.13.0 avoids depending on a newer torch version's
   legacy-tracer fallback at all, which is closer to what the officially
   shipped `vits-mms-rus`/`vits-mms-ukr` packages were built with, so it's
   what's actually deployed here).
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
4. Run the unmodified export script from
   `https://huggingface.co/csukuangfj/vits-mms-rus/raw/main/vits-mms.py` with
   `PYTHONPATH` including the cloned `MMS` and `MMS/vits` dirs and
   `language=<lang>` set.
5. Produces `model.onnx` + `tokens.txt`; copy to
   `$SB_MODELS_DIR/tts_vits/vits-mms-{uzb,kaa}/`.

Both `uzb` and `ukr` (and evidently other MMS checkpoints trained via the
wav2vec2/CTC-style pipeline) embed `|` as vocab id 0 and a literal space as a
separate, late entry — cosmetic labeling of what's functionally just the
`add_blank` pad slot (see #15); no vocab.txt editing or space/pipe
substitution is needed, confirmed by diffing against the officially published
`vits-mms-ukr.tar.bz2`, which has the same shape and works correctly
unmodified.

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

---

## 14. Real-time dispatch stalled indefinitely on unpunctuated speech

**Status:** fixed.

Blocker #12's real-time sentence dispatch (`Session::HandleEvents`) only
flushed a sentence once `text::SplitSentences` found sentence-ending
punctuation in the growing PARTIAL. That's fine for clean, punctuated
narration (the `parakeet.cpp` test fixture, or the `en->ru` e2e clip) — but
streaming ASR punctuation restoration is unreliable mid-utterance, especially
for non-English source languages, and normal continuous/conversational
speech routinely runs many seconds without producing one. With no boundary
ever found, `sentences.size()` never grows past what's already dispatched,
and the session falls all the way back to waiting for parakeet's own `<EOU>`
(trained for turn-taking pauses, not sentence breaks — see #5) or `Stop()`.
From the user's seat that reads as "nothing translates/speaks until I press
Stop" — reproduced end-to-end with a Playwright session against the real
server: default settings (`source=ru`, `target=uz`, the app's own load-time
default) speaking English test audio (forcing `ru` phonetic decoding, which
produces text with essentially no sentence-final punctuation) — zero
`transcript` events fired for the whole 15s listening window, one arrived
only once `Stop` triggered `Finish()`.

**Fix:** `Session::DispatchGrowingText` (renamed from the old
sentence-count-based logic; `server/src/httpapi/session.{h,cpp}`) now tracks
`dispatched_len_`, a byte offset into the growing utterance text already
handed to `Dispatch()`, instead of a sentence-array index. On top of the
existing punctuation-boundary dispatch, it also force-flushes whatever
undispatched tail exists once that tail has sat open for
`kMaxChunkLatencyMs` (3000 ms) of audio time, regardless of punctuation —
bounding worst-case latency to one rolling ~3s chunk instead of an entire
unbounded utterance. This is the same trade-off real-time speech-to-speech
products (e.g. Samsung's Live Translate) make: translate in short rolling
windows when a clean sentence boundary doesn't show up in time, rather than
waiting indefinitely for one.

One subtlety this introduced and fixed in the same pass: once a fallback
chunk cuts a sentence mid-way, the *next* partial's newly-punctuated version
of that same sentence would otherwise re-match and get dispatched a second
time (spoken twice — worse than the latency problem it fixes). Fixed by only
treating a found sentence as wholly new when it *starts* at or after
`dispatched_len_`; a sentence that only partially overlaps already-dispatched
text is absorbed into `dispatched_len_` without a second `Dispatch()` call
(silently drops the last couple of trailing words of that one sentence's
translation — an acceptable trade for never repeating audio out loud).

**Verified** with real audio through the actual WS session (not just unit
logic): a Playwright test against a long, deliberately unpunctuated ~15s
English run-on synthesized via `/v1/speak` (`lang=en`, magpie) now dispatches
transcript → translation → audio roughly every 3s, all well before `Stop` is
pressed, with no duplicate/overlapping audio. Re-ran the original punctuated
fixture afterward to confirm no regression (still dispatches on sentence
boundaries where they exist, still no duplicates). `make test-e2e` and the
unit suite (`sb_tests`, 109 assertions) both still pass.

---

## 15. Live-session TTS spoke the caption text, not the translation

**Status:** fixed.

User report after #14 landed: "hear voice but not human voice, very short,
almost doesn't work." Reproduced with real audio through `/v1/speak`: a
~70-character Uzbek sentence in **Cyrillic** (what MADLAD actually emits, and
what the `uz`/`kaa` MMS voices are trained on — see `models/MANIFEST.md`'s
"MADLAD emits Uzbek in Cyrillic" note) played correctly at ~5.5s, matching the
known-good `ru` voice's rate for similar-length text. The same sentence
**transliterated to Latin** played at ~0.7s — a ~7x reduction, garbled,
because `sherpa-onnx`'s character frontend
(`offline-tts-character-frontend.cc::ConvertTextToTokenIds`) silently drops
any character not in the voice's vocab, and a Cyrillic-only vocab contains no
Latin letters at all: nearly the entire sentence vanishes before synthesis,
leaving a token stream of little more than spaces and punctuation.

First chased this down the wrong path — spent real effort re-exporting both
voices with `torch==1.13.0+cpu` (see #6) suspecting the export itself, since
the initial verification of #6 tested `/v1/speak` with **hand-typed Latin**
Uzbek text and never caught that the `uz` MMS voice needs Cyrillic. Confirmed
the export was never the problem by A/B testing against the officially
published `vits-mms-ukr.tar.bz2` (same "|"-at-id-0 vocab shape, same
pipeline) swapped in as a temporary stand-in for `uz` — it produced the exact
same abnormally short duration for the same (Latin) test input, proving the
bug was in what text reached the frontend, not in either export.

Root cause, once found: `Session::Dispatch`
(`server/src/httpapi/session.cpp`) reused one variable for two different
jobs. MT's raw output is Cyrillic; the code did
`translated = pipeline::ForDisplay(out.text, target_lang)` — converting to
Latin for the on-screen caption — and then passed that *same, now-Latin*
`translated` into `Synth(ForTTS(translated, ...), ...)` for the actual audio.
`ForDisplay` only transliterates for `uz`, and `uz` TTS had no real voice
until #6, so this bug was latent from the original Go implementation
(`app/internal/session/session.go`, pre-dating the C++ rewrite) — copied
faithfully across the C++ port — never exercised until a working `uz` voice
existed to expose it. The batch REST path
(`server/src/pipeline/pipeline.cpp`'s speech-to-speech handler) keeps the raw
MT text (`out`) and the display text (`display`) as separate variables
already and was never affected.

**Fix:** `Dispatch` now keeps `spoken` (MT's raw output, passed to `Synth`)
and the caption message's `ForDisplay`'d text as two independent values —
comment added explaining why they must never be merged back into one
variable. Verified via `/v1/speak` with real Cyrillic input (duration back to
~5.5s for a ~70-char sentence, matching `ru`) and via a full live WS session
with the app's actual default settings (`ru`→`uz`): audio sample counts now
scale with translated-sentence length (e.g. 43409 samples / 16kHz = 2.71s for
a 34-character sentence — before the fix, every sentence regardless of length
was well under 1s). `make test-e2e` and `sb_tests` (109 assertions) pass.

---

## 16. GPU backends: CUDA added; MT's Metal wiring was never actually live

**Status:** stt/mt/tts_magpie: CUDA and Metal both wired, CPU-verified only
(no GPU hardware in development — same caveat as #4). tts_vits: CPU-only on
every backend, by design (see below).

Added `SB_CUDA` (mirrors the existing `SB_METAL`) to `cores/{stt,mt,tts_magpie}`'s
CMakeLists, forwarding to each vendored engine's own CUDA option
(`PARAKEET_GGML_CUDA`, `GGML_CUDA` directly for llama.cpp, `MAGPIE_GGML_CUDA`)
— all three already anticipated this cleanly, no vendored-file edits needed.
`scripts/build.sh --cuda` (Linux only; `--metal` and `--cuda` are mutually
exclusive) and three new run scripts, one per target:
`scripts/run-{cpu,metal,cuda}.sh` — each checks its own prerequisites (right
OS/arch, `xcrun --find metal` / `nvcc` + `nvidia-smi`) and refuses with a
clear message rather than attempting a build doomed to fail. `doctor.sh` now
reports CUDA toolkit + driver presence on Linux the same way it already
reports the Metal toolchain on macOS.

**Found while wiring this: MT never actually used Metal, despite #4 saying it
was "wired."** `cores/mt/sb_mt.cpp`'s `sb_mt_model_load` had
`mp.n_gpu_layers = 0` hardcoded — compiling llama.cpp's ggml with
`GGML_METAL=ON` makes the backend *available*, but llama.cpp still needs
`n_gpu_layers > 0` to actually offload anything to it, and that was pinned to
zero unconditionally. So even a full-Xcode `--metal` build ran MT on CPU the
entire time. `sb_stt.cpp` (parakeet.cpp) and `sb_tts_magpie.cpp`
(magpie-tts.cpp) don't have this problem: both engines' `Backend` class
enumerates `ggml_backend_dev_*` at runtime and auto-selects the first
non-CPU device on its own — no per-call layer count to wire — so Metal (and
now CUDA) already worked for STT and magpie-TTS as soon as #4 compiled them
in; MT was the one silent gap.

**Fix:** `sb_mt_model_load` gained a `device` parameter (`cpu|metal|cuda|auto`,
matching `sb_stt_model_load`/`sb_tts_model_load`'s existing convention) and
sets `n_gpu_layers` to `999` (llama.cpp's own idiom for "offload everything
that exists" — internally clamped to the model's real layer count) for
anything but `"cpu"`. This is a breaking change to a public core function
signature, so `SB_ABI_VERSION` bumped 1 → 2 (`cores/common/sb_abi.h`) and
`cores/smoke/sb_smoke.c`'s hardcoded expectation moved with it. `SB_DEVICE`
(`server/src/config.cpp`) now accepts `cuda` alongside `cpu|metal|auto`.
Verified: full clean rebuild, symbol check, one-process dlopen smoke test (all
four cores report `abi_version() == 2`), `sb_tests` (109 assertions),
`make test-e2e`, plus a live server round-trip (`/v1/speak`,
`/v1/speech-to-speech`) — all CPU-only, on this Linux x86_64 host.

**tts_vits (sherpa-onnx) stays CPU-only on every backend, deliberately — not
an oversight.** sherpa-onnx's own CUDA path (`SHERPA_ONNX_ENABLE_GPU`) needs a
GPU onnxruntime build that *requires* `BUILD_SHARED_LIBS=ON`
(`third_party/sherpa-onnx/CMakeLists.txt` forces it), which conflicts with
this core's static-link isolation — the same isolation #11's onnxruntime
1.18.1 pin already depends on being intact. It would also mean fetching yet
another, much larger onnxruntime archive (the GPU build) alongside the CPU
one already vendored there. `cores/tts_vits/CMakeLists.txt` declares `SB_CUDA`
as an accepted-but-unused option (matching its existing `SB_METAL` line) so
`--cuda`/`--metal` builds don't error on this core; it just keeps synthesizing
on CPU. Net effect in a `--cuda`/`--metal` build: STT, MT, and magpie-TTS (the
`ar/de/en/es/fr/hi/it/ko/pt-BR/vi` targets) get GPU acceleration; the VITS
voices (`ru/uz/kaa`) do not.
