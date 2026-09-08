# Models

See `../models/MANIFEST.md` for exact files, URLs and sha256 sums, and
`scripts/fetch-models.sh` to download + verify them. Default quant set: **q4_k**
(fits an 8 GB-RAM host with STT + MT + TTS all resident).

| core | model | covers | script |
|------|-------|--------|--------|
| stt | Nemotron 3.5 ASR streaming 0.6B (GGUF) | uz, ru (kaa unverified); streaming + EOU | any |
| mt | MADLAD-400 3B MT (T5, GGUF) | 450+ langs via `<2xx>` tag | any |
| tts_magpie | Magpie TTS Multilingual 357M (GGUF) | en, de, es, fr, it, pt-BR, hi, ko, vi, ar | Latin |
| tts_vits | MMS-TTS VITS (sherpa-onnx) | ru now; uz, kaa deferred (see blockers.md #6) | Cyrillic |

## Uzbek script handling

MADLAD-400 emits Uzbek in **Cyrillic**. The MMS Uzbek voice
(`facebook/mms-tts-uzb-script_cyrillic`) also expects **Cyrillic**. Therefore:

- **TTS is fed Cyrillic directly** — no transliteration on the synthesis path.
- **Captions/display for `target=uz` are transliterated to Latin** by
  `app/internal/text` (handles o`/g`/sh/ch/ng and the hard sign).

This keeps transliteration off the latency-critical audio path and confined to
display text.

## STT prompt / language

`nemotron-3.5-asr-streaming-0.6b` is prompt-conditioned: `sb_stt_stream_new`
passes the source language (`ru`|`uz`|`kaa`|`auto`) which parakeet.cpp turns
into the model prompt via `parakeet_capi_stream_begin_lang`.
