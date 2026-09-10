# Speech Bridge

Production-ready, fully local, real-time **speech-to-speech translation** in
Go + C++. Live meeting interpretation (speaker in Russian → listener hears an
Uzbek voice + live captions) and batch mode (WAV → transcript, translation,
synthesized audio).

Everything runs locally: no cloud APIs, no Python at build or runtime, no
PyTorch, no CUDA toolkit. CPU-first; Metal on Apple Silicon.

**Platforms (both first-class):** Linux x86_64, macOS arm64.
cgo forbids cross-compilation — build natively on each.

## How it works

```
mic / WAV ─► STT (parakeet) ─► sentence split ─► MT (MADLAD) ─► TTS ─► PCM out
             streaming           (pure Go)        per sentence    magpie │ vits
             partial + EOU                                        by target lang
```

One process `dlopen`s four self-contained shared libraries and runs the whole
pipeline in-memory — no HTTP between stages. Each core statically links its own
engine **and its own ggml** (the three ggml copies are mutually incompatible
and must never share a link unit). See `ARCHITECTURE.md`, `third_party/PINNED.md`.

| core               | engine        | role | script |
|--------------------|---------------|------|--------|
| `libsb_stt`        | parakeet.cpp  | streaming STT (uz/ru/kaa), end-of-utterance events | — |
| `libsb_mt`         | llama.cpp     | MT — MADLAD-400 3B (T5), `<2xx> text`, 512-token ctx | — |
| `libsb_tts_magpie` | magpie-tts.cpp| TTS for en/de/es/fr/it/pt-BR/hi/ko/vi/ar | Latin |
| `libsb_tts_vits`   | sherpa-onnx   | TTS (MMS/VITS) for uz/ru/kaa | Cyrillic |

## Build

Prereqs: CMake ≥ 3.20, a C++17 compiler, Go ≥ 1.23, GNU Make, bash.
Run `make doctor` to check the environment.

### Linux x86_64
```sh
sudo apt install build-essential cmake golang         # gcc ≥ 12
make doctor
./scripts/build.sh                                    # cores + symbol check + smoke + go build
#   -> lib/libsb_*.so   (release builds pin AVX2+FMA, no -march=native)
#   -> app/sb-server
```

### macOS arm64
```sh
xcode-select --install
brew install cmake go
make doctor
./scripts/build.sh                                    # CPU-first
./scripts/build.sh --metal                            # + Metal ggml backend (needs full Xcode)
#   -> lib/libsb_*.dylib
#   -> app/sb-server
```

### Models (~3.5 GB, architecture-independent)
```sh
make fetch-models                                     # sha256-verified against models/MANIFEST.md
```
Uzbek/Karakalpak VITS voices need a one-off offline export — see
`models/MANIFEST.md` and `docs/blockers.md` #6.

## Run
```sh
make run                                              # SB_BIND defaults to 127.0.0.1:8080
```
- `GET  /`                     web test UI (mic → live captions + translated voice)
- `GET  /health /ready /metrics /v1/capabilities`
- `POST /v1/speech-to-speech`  multipart WAV → transcript + translation + audio
- `POST /v1/transcribe /v1/translate /v1/speak`
- `WS   /v1/stream`            live speech → ordered partial/transcript/translation/audio

Full contract: `docs/api.md`. Deploy: `docs/operations.md`.

## Test

```sh
make test        # unit tests — no models, no native libs
make check       # build cores → check-symbols → one-process dlopen smoke test → go vet → go test
make test-e2e    # real cores + real models (skips cleanly if models absent)
make bench       # latency budget against a running server
```

## Status

Milestones 1–8 implemented. Verified on macOS arm64 (CPU-only, 8 GB): all four
cores build with `sb_*`-only exports, the one-process smoke test passes, real
STT transcription is clean, VITS + magpie synthesize real audio, batch and
streaming both work end-to-end. Known gaps and deviations: `docs/blockers.md`
(no public Uzbek STT fine-tune → base multilingual nemotron; uz/kaa VITS voices
deferred to an offline export; Metal wired but unverified without full Xcode).
