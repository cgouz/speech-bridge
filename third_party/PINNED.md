# Vendored inference engines — provenance

All trees under `third_party/` are vendored **whole** and are **read-only**.
Never edit files here. Integration code lives in `cores/*/`. See
`../docs/blockers.md` for any deviations.

Vendored on: 2026-09-08
Host at vendoring time: Darwin arm64 (macOS)

| name                       | origin                                          | commit                                     | commit date  |
|----------------------------|-------------------------------------------------|--------------------------------------------|--------------|
| `parakeet.cpp`             | https://github.com/mudler/parakeet.cpp.git      | `e75de9b6b9b688fd293aa22f7e27aa724ea286f8` | 2026-08-18   |
| `parakeet.cpp/third_party/ggml` (submodule) | https://github.com/ggml-org/ggml   | `e705c5fed490514458bdd2eaddc43bd098fcce9b` | (submodule pin) |
| `llama.cpp`                | https://github.com/ggml-org/llama.cpp.git       | `f3f1a8f2760f28325a5ec20c05b171e5b7c83a29` | 2026-09-08   |
| `magpie-tts.cpp`           | https://github.com/mudler/magpie-tts.cpp.git    | `3008ff73fc2d2da9e4d743b09350aa7023e8980c` | 2026-07-24   |
| `magpie-tts.cpp/third_party/ggml` (submodule) | https://github.com/ggml-org/ggml | `e705c5fed490514458bdd2eaddc43bd098fcce9b` | (submodule pin) |
| `sherpa-onnx`              | https://github.com/k2-fsa/sherpa-onnx.git       | `fe56f3dec6bbcab54f885d64d7d4c63862cdcfe2` | 2026-09-08   |

## ggml copies are intentionally NOT shared

`parakeet.cpp` and `magpie-tts.cpp` both pin ggml
`e705c5fed490514458bdd2eaddc43bd098fcce9b`, and `llama.cpp` bundles its own
in-tree ggml at a different revision. They are **mutually incompatible**:

- `parakeet.cpp` applies four in-tree ggml patches (see below).
- `magpie-tts.cpp` builds ggml with `GGML_MAX_NAME=128` (default is 64);
  its GGUF tensor names are up to 102 chars and load as garbage otherwise.
- `llama.cpp`'s ggml is a newer revision with its own history.

Each core (`libsb_stt`, `libsb_mt`, `libsb_tts_magpie`, `libsb_tts_vits`)
statically links its **own** engine and its **own** ggml into a
self-contained shared library. They are never combined into one link unit.

## Vendoring steps performed

1. Recorded origin + HEAD for each clone (table above).
2. `git submodule update --init --recursive` in `parakeet.cpp` and
   `magpie-tts.cpp` to materialize their `third_party/ggml` trees, so each
   engine is vendored **with** its exact ggml.
3. Moved each entire tree to `third_party/<name>/`.
4. Deleted every inner `.git` directory and the (now dangling) submodule
   `.git` pointer files.
5. Pre-applied `parakeet.cpp`'s four `third_party/ggml-patches/*.patch` to
   its vendored `third_party/ggml` (its own build system would apply these
   at configure time via `scripts/apply_ggml_patches.sh`, which needs a git
   repo we removed). The patch files are retained unmodified for audit.
   See `../docs/blockers.md` — "parakeet ggml patches".
   Patches, in order:
   - `0001-ggml-cpu-fold-broadcast-iterations-in-llamafile_sgem.patch`
   - `0002-metal-conv-2d-dw.patch`
   - `0003-metal-pad-leading.patch`
   - `0004-cuda-pad-grid-stride.patch`
6. sherpa-onnx was not present in `temp/`; cloned fresh (`--depth 1`) into
   `third_party/` and pinned above.

## Trimming

No vendored tree has been trimmed. Trimming (`examples/`, `docs/`,
`tests/`) is permitted only as a separate step after all cores build and
pass the one-process smoke test, and only for directories provably unused
by the build.
