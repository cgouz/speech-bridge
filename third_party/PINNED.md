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

No vendored inference-engine tree has been trimmed. Trimming (`examples/`,
`docs/`, `tests/`) is permitted only as a separate step after all cores
build and pass the one-process smoke test, and only for directories
provably unused by the build.

---

# Vendored application libraries — the `pure-cpp` server

Everything below backs the C++ rewrite of the Go orchestrator (HTTP/WS
server, JSON, unit tests). Same rule: read-only, vendored whole or as an
official amalgamated distribution, never hand-edited.

Vendored on: 2026-09-11

| name              | origin                                             | commit / tag                                | notes |
|-------------------|-----------------------------------------------------|----------------------------------------------|-------|
| `uWebSockets`      | https://github.com/uNetworking/uWebSockets.git      | `v20.80.0`                                    | header-only C++17; trimmed to `src/` + `uSockets/` (see below) |
| `uWebSockets/uSockets` | https://github.com/uNetworking/uSockets.git     | `86097c490263ab662d62e8e7b541390bdec7d149`    | uWebSockets v20.80.0's pinned submodule commit; trimmed to `src/` (no TLS/QUIC — see below) |
| `nlohmann_json`    | https://github.com/nlohmann/json.git                | `v3.12.0`                                     | official single-header amalgamation (`single_include/nlohmann/json.hpp`), not a full clone |
| `catch2`           | https://github.com/catchorg/Catch2.git              | `v3.16.0`                                     | official two-file amalgamation (`extras/catch_amalgamated.{hpp,cpp}`), not a full clone |

## uWebSockets / uSockets trimming

Vendored whole at v20.80.0, then trimmed to only what our server needs:

- Removed (uWebSockets): `autobahn/`, `benchmarks/`, `build.c`, `build.h`,
  `cluster/`, `examples/`, `fuzzing/`, `GNUmakefile`, `h1spec/` (unpopulated
  submodule), `libdeflate/` (unpopulated submodule — permessage-deflate
  compression, unused), `libEpollBenchmarker/`, `misc/`, `tests/`, `Makefile`
  (we build via our own CMake, not uWebSockets' Makefile).
- Removed (uSockets): `boringssl/` and `lsquic/` (unpopulated submodules —
  TLS and HTTP/3/QUIC, both unused: the server binds `127.0.0.1`/an internal
  network only, matching the existing Go server's plain-HTTP posture),
  `examples/`, `misc/`, `tests/`, `Makefile`, `module.modulemap`.
- Kept: `uWebSockets/src/*.h` (all headers) and `uWebSockets/uSockets/src/`
  in full (`crypto/`, `io_uring/`, `quic.c/h`, and the `libuv`/`asio`/`gcd`
  eventing backends stay vendored but are simply not compiled — see
  `cores/server_cpp/CMakeLists.txt`, which builds only `bsd.c`, `context.c`,
  `loop.c`, `socket.c`, `udp.c`, and `eventing/epoll_kqueue.c` — the third
  eventing backend, `#ifdef`-selected between epoll (Linux) and kqueue
  (macOS/BSD), matching this project's other two target platforms).

## nlohmann_json / catch2

Both projects publish an official amalgamated distribution specifically for
vendoring (one/two files, no build system, no test suite) — used instead of
a full clone, consistent with "vendor exactly what integration needs."
