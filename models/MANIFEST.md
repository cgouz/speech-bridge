# Model manifest

`scripts/fetch-models.sh` reads the machine-readable block below, downloads each
entry into `$SB_MODELS_DIR` (default `models/`), and verifies its sha256.
Weights are **gitignored** — never commit them. Files are architecture-
independent: the same downloads serve Linux x86_64 and macOS arm64.

Default set is **q4_k** (chosen for 8 GB-RAM hosts: STT + MT + TTS all resident
at once). Alternate quants are listed under "Other quants" for reference.

## Layout produced

```
$SB_MODELS_DIR/
├── stt/nemotron-3.5-asr-streaming-0.6b-q4_k.gguf   -> SB_STT_MODEL
├── mt/madlad400-3b-mt-q4_k.gguf                     -> SB_MT_MODEL
├── tts_magpie/magpie-tts-multilingual-357m-q4_k.gguf -> SB_TTS_MAGPIE_MODEL
└── tts_vits/                                        -> SB_TTS_VITS_DIR
    └── vits-mms-rus/   (model.onnx, tokens.txt)
```

## Fetch set (parsed by scripts/fetch-models.sh)

Fields, tab-separated: `file` · `dest` (path relative to `$SB_MODELS_DIR`) ·
`sha256` · `bytes` · `url`.

<!-- SB-MANIFEST-BEGIN -->
```
file	stt/nemotron-3.5-asr-streaming-0.6b-q4_k.gguf	5ad85eb3f3014c1a300d67b7ccbd23c38c4c952405cbe33a861e19fb2775e84b	718102624	https://huggingface.co/mudler/parakeet-cpp-gguf/resolve/main/nemotron-3.5-asr-streaming-0.6b-q4_k.gguf
file	mt/madlad400-3b-mt-q4_k_m.gguf	fc56f16d215db71e856de3c3770974c867e3d95a782d415d4cfabc9fb470b8e4	1858124864	https://huggingface.co/mtsdurica/madlad400-3b-mt-Q4_K_M-GGUF/resolve/main/madlad400-3b-mt-q4_k_m.gguf
file	tts_magpie/magpie-tts-multilingual-357m-q4_k.gguf	af852e23862be7def7becfbe38df75e6a23e4b9af27a73d1d63431614a9d2fd4	540839648	https://huggingface.co/mudler/magpie-tts.cpp-gguf/resolve/main/magpie-tts-multilingual-357m-q4_k.gguf
file	tts_vits/vits-mms-rus/model.onnx	8735fad753674ec67c084ef78dc7d3fa47aacceb12e7518d05e0e11db1ed34dd	114021556	https://huggingface.co/csukuangfj/vits-mms-rus/resolve/main/model.onnx
file	tts_vits/vits-mms-rus/tokens.txt	361fb8874d234fee4893091334ffcdd10a30367dc7bee725d5f771eeb44ed23b	447	https://huggingface.co/csukuangfj/vits-mms-rus/resolve/main/tokens.txt
```
<!-- SB-MANIFEST-END -->

## Model notes

| core        | model                                   | source | license |
|-------------|-----------------------------------------|--------|---------|
| stt         | NVIDIA Nemotron 3.5 ASR streaming 0.6B (GGUF by mudler) | [nvidia/nemotron-3.5-asr-streaming-0.6b](https://huggingface.co/nvidia/nemotron-3.5-asr-streaming-0.6b) · [mudler/parakeet-cpp-gguf](https://huggingface.co/mudler/parakeet-cpp-gguf) | OpenMDW-1.1 |
| mt          | Google MADLAD-400 3B MT (T5; GGUF by cstr) | [google/madlad400-3b-mt](https://huggingface.co/google/madlad400-3b-mt) · [cstr/madlad400-3b-mt-GGUF](https://huggingface.co/cstr/madlad400-3b-mt-GGUF) | Apache-2.0 |
| tts_magpie  | NVIDIA Magpie TTS Multilingual 357M (GGUF by mudler) | [nvidia/magpie_tts_multilingual_357m](https://huggingface.co/nvidia/magpie_tts_multilingual_357m) · [mudler/magpie-tts.cpp-gguf](https://huggingface.co/mudler/magpie-tts.cpp-gguf) | NVIDIA OpenModel |
| tts_vits    | Meta MMS-TTS VITS, Russian (sherpa-onnx build) | [k2-fsa/sherpa-onnx releases: tts-models](https://github.com/k2-fsa/sherpa-onnx/releases/tag/tts-models) | CC-BY-NC-4.0 |

- **STT is the base multilingual model, not an Uzbek fine-tune.** The mission
  names a "nemotron uz-streaming fine-tune (uz, ru, kaa)"; no such checkpoint is
  published. `nemotron-3.5-asr-streaming-0.6b` is prompt-conditioned across 40+
  locales (incl. uz, ru) with cache-aware streaming + EOU. Karakalpak (kaa)
  coverage is unverified. Swap in a real uz fine-tune here if one becomes
  available.
- **MADLAD emits Uzbek in Cyrillic.** `facebook/mms-tts-uzb-script_cyrillic`
  confirms the MMS uz voice also expects **Cyrillic** — so TTS is fed Cyrillic
  directly; `app/internal/text` transliterates to Latin only for on-screen
  captions. (See `../docs/models.md`.)
- **magpie covers none of uz/ru/kaa** (en, de, es, fr, it, pt-BR, hi, ko, vi,
  ar + 3 Arabic variants). It is unused for the ru→uz meeting path; it serves
  target languages in its set.

## Deferred: Uzbek & Karakalpak VITS voices

sherpa-onnx ships a pre-built package only for Russian MMS. `facebook/mms-tts-uzb-script_cyrillic`
and `facebook/mms-tts-kaa` exist but in HF-Transformers format; converting to the
sherpa-onnx VITS layout is a one-off **offline** step (sherpa's
`scripts/mms/export-onnx-mms.py`, Python — not part of build or runtime). Tracked
in `../docs/blockers.md`. Until then `/v1/capabilities` reports uz/kaa TTS as
unavailable and the pipeline degrades to captions-only for those targets.

Expected final layout once prepared:
```
tts_vits/vits-mms-uzb/   (model.onnx, tokens.txt)   # Cyrillic
tts_vits/vits-mms-kaa/
```

## Other quants (not fetched by default)

| file | sha256 | bytes |
|------|--------|-------|
| `nemotron-3.5-asr-streaming-0.6b-q5_k.gguf` | `7bb14605a707f821560952521034002c91653d676272a80dca0cd2aa886d0ab2` | 784801888 |
| `nemotron-3.5-asr-streaming-0.6b-q8_0.gguf` | `ba2f13eccd4a5245be728f77e6149bd6a4fdcdd133ff2e08ac6005bcef7a99f1` | 983696512 |
| `realtime_eou_120m-v1-q8_0.gguf` (English EOU only) | `62616b914d6f5a683a5dea672df055b57de5c49dddf871b8b44b9c814dc3d896` | 176001472 |
| `madlad400-3b-mt-q8_0.gguf` | `d3ae3845ccc441474f1cc57542fe2885edc7946aec73c7134f971b20b64d68b5` | 3375619904 |
| `magpie-tts-multilingual-357m-q6_k.gguf` | `8291ffde2e13e2e9221a000669b5f7814c7ecc858eb0a1a9de8ee77d8da05736` | 584437728 |
| `magpie-tts-multilingual-357m-q8_0.gguf` | `0f26f552c82b45a176bd7b7de261581821d5e81acc99cedb4450fd20a10ad259` | 624287072 |
