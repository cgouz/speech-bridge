# API

HTTP + WebSocket surface of `sb-server`. This document is kept in sync with
actual behavior (mission: "docs/api.md matches actual behavior"). Endpoints are
implemented across milestones 5–7; this is the contract they target.

## Batch REST

- `POST /v1/speech-to-speech` — multipart WAV -> `{request_id, transcript,
  translation, audio(base64), audio_format, sample_rate,
  timings{stt_ms,mt_ms,tts_ms,total_ms}, stage}`
- `POST /v1/transcribe`, `POST /v1/translate`, `POST /v1/speak`
- `GET /v1/capabilities` — supported languages per TTS engine
- `GET /health` (liveness), `GET /ready` (all loaded cores respond)
- `GET /metrics` (Prometheus), `GET /` (web UI)

Degraded results return 200 with `stage: "mt_unavailable" | "tts_unavailable"`.
Empty transcript -> 200 empty result. STT unavailable -> 503.

Error shape: `{"error":{"code","message","stage","request_id"}}` with stable
machine-readable codes (enumerated here in milestone 5).

## WebSocket `/v1/stream`

One session = one speaker -> one target language.

Client -> server: JSON `{"type":"start","source_lang","target_lang","voice",
"sample_rate","format":"f32|s16"}`, then binary PCM frames (~250 ms), then
`{"type":"stop"}`.

Server -> client: `partial`, `transcript` (on EOU), `translation`, `audio`
(JSON header + one binary PCM frame), `error` (per-sentence, non-fatal).
`seq` = sentence index; audio emitted in seq order via a reorder buffer.

## Config (env, `SB_` prefix, validated at startup)

`SB_BIND` `SB_MODELS_DIR` `SB_LIB_DIR` `SB_STT_MODEL` `SB_MT_MODEL`
`SB_TTS_MAGPIE_MODEL` `SB_TTS_VITS_DIR` `SB_MT_CTX=512` `SB_MAX_BODY_MB=25`
`SB_MAX_AUDIO_SEC=120` `SB_QUEUE_DEPTH=8` `SB_STREAMS_MAX=4` `SB_AUTH_TOKEN`
`SB_LOG_LEVEL` `SB_LOG_FORMAT=json`
