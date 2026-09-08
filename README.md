# Speech Bridge

Production-ready, fully local, real-time **speech-to-speech translation** in
Go + C++. Live meeting interpretation (speaker in Russian → listener hears an
Uzbek voice + live captions) and batch mode (WAV → transcript, translation,
synthesized audio).

Everything runs locally: no cloud APIs, no Python at build or runtime, no
PyTorch, no CUDA toolkit. CPU-first; Metal on Apple Silicon.

**Platforms (both first-class):** Linux x86_64, macOS arm64.
cgo forbids cross-compilation — build natively on each.

## Engines (vendored, read-only, in `third_party/`)

| core               | engine            | role |
|--------------------|-------------------|------|
| `libsb_stt`        | parakeet.cpp      | streaming STT (uz/ru/kaa), emits end-of-utterance events |
| `libsb_mt`         | llama.cpp         | MT — MADLAD-400 3B (T5), `<2xx> text`, 512-token ctx |
| `libsb_tts_magpie` | magpie-tts.cpp    | TTS for en/de/es/fr/it/pt-BR/hi/ko/vi/ar |
| `libsb_tts_vits`   | sherpa-onnx       | TTS (MMS-TTS/VITS) for uz/ru/kaa |

Each core is a **self-contained shared library** that statically links its own
engine and its own ggml — the three ggml copies are mutually incompatible and
must never share a link unit. See `ARCHITECTURE.md` and `third_party/PINNED.md`.

## Build

### Prerequisites
- CMake ≥ 3.20, a C++17 compiler
- Go ≥ 1.22
- GNU Make, bash
- Linux: gcc ≥ 12 or clang ≥ 15
- macOS: Xcode Command Line Tools (full Xcode for `--metal`)

Run `make doctor` to check.

### Linux x86_64
```sh
make doctor
make cores          # -> lib/libsb_*.so   (portable AVX2+FMA baseline on --release)
make app            # -> app/sb-server
make check-symbols  # every lib exports only sb_*
```

### macOS arm64
```sh
make doctor
make cores          # -> lib/libsb_*.dylib   (add: scripts/build.sh --metal, needs full Xcode)
make app            # -> app/sb-server
make check-symbols
```

### Models
```sh
make fetch-models   # downloads the q4_k set (~3.4 GB) + Russian VITS, sha256-verified
```
Fetched into `models/{stt,mt,tts_magpie,tts_vits}/`. Uzbek/Karakalpak VITS
voices need a one-off offline export — see `models/MANIFEST.md` and
`docs/blockers.md` #6.

## Run
```sh
make run                     # starts sb-server (default 127.0.0.1:8080)
# GET /            web test UI (mic -> captions + audio)
# GET /health /ready /metrics /v1/capabilities
# POST /v1/speech-to-speech  (multipart WAV)
# WS  /v1/stream              live speech -> captions + translated voice
```

## Status

Milestone 1 complete: engines vendored (`third_party/PINNED.md`), repo skeleton,
`libsb_stt` builds against vendored parakeet.cpp and passes `make check-symbols`.
Remaining milestones tracked in the mission spec. Deviations: `docs/blockers.md`.
