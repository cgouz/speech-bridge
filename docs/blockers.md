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

**Status:** deferred (milestone 8); TTS degrades to captions-only for uz/kaa.

`libsb_tts_vits` (sherpa-onnx / MMS-TTS) is meant to cover uz, ru, kaa — the
languages magpie lacks. sherpa-onnx publishes a pre-built package only for
Russian (`vits-mms-rus`, in the `tts-models` GitHub release). The upstream
weights for Uzbek (`facebook/mms-tts-uzb-script_cyrillic`) and Karakalpak
(`facebook/mms-tts-kaa`) exist but only in HF-Transformers format.

Converting to the sherpa-onnx VITS layout (`model.onnx` + `tokens.txt` +
`config.json`) needs sherpa's `scripts/mms/export-onnx-mms.py` — **Python**.
That is an offline, one-time model-prep step (not part of build or runtime, so
it does not violate "no Python anywhere in build or runtime"), but it is not yet
done here.

**Interim behavior:** `fetch-models.sh` fetches ru only. `/v1/capabilities` and
`/ready` report uz/kaa TTS unavailable; the pipeline runs captions-only for
those targets (degraded mode, per spec). The primary ru→uz meeting demo needs
the uz voice, so this gap must close in milestone 8.

**Recipe (milestone 8):** in a throwaway venv, run sherpa's
`export-onnx-mms.py` for `uzb` (cyrillic) and `kaa`, drop the outputs under
`$SB_MODELS_DIR/tts_vits/vits-mms-{uzb,kaa}/`, add `file` entries with sha256 to
`models/MANIFEST.md`.

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
