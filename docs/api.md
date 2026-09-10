# API

HTTP + WebSocket surface of `sb-server`. This document tracks the implemented
behavior.

Base URL defaults to `http://127.0.0.1:8080` (`SB_BIND`). When `SB_AUTH_TOKEN`
is set, every `/v1/*` endpoint except `/v1/capabilities` requires
`Authorization: Bearer <token>`. `/health`, `/ready`, `/metrics` and
`/v1/capabilities` are always open (capabilities is read before a client has a
token, to populate a setup UI).

`/v1/stream` also accepts the token as an `access_token` query parameter
(`wss://host/v1/stream?access_token=<token>`), since browsers cannot set a
custom header on a WebSocket handshake. The `Authorization` header still works
too (e.g. for non-browser clients) and is checked first.

Every response carries an `X-Request-ID` header; it also appears as
`request_id` in JSON bodies and in the error shape.

## Error shape

```json
{ "error": { "code": "…", "message": "…", "stage": "…", "request_id": "…" } }
```

`stage` is present when the failure is attributable to a pipeline stage
(`stt` | `mt` | `tts`).

| code | HTTP | meaning |
|------|------|---------|
| `bad_request` | 400 | missing/invalid field or JSON body |
| `bad_audio` | 400 | body is not a valid WAV (validated by header) |
| `audio_too_long` | 413 | clip longer than `SB_MAX_AUDIO_SEC` |
| `body_too_large` | 413 | request body over `SB_MAX_BODY_MB` |
| `unauthorized` | 401 | missing/invalid bearer token |
| `queue_full` | 429 | request queue saturated — retry after `Retry-After` seconds |
| `stt_unavailable` | 503 | STT engine not loaded |
| `mt_unavailable` | 503 | MT engine not loaded (only for `/v1/translate`) |
| `tts_unavailable` | 503 | no TTS engine covers the requested language |
| `stt_error` / `mt_error` / `tts_error` | 500 | inference failed |
| `pipeline_error` | 500 | unexpected pipeline failure |
| `internal` | 500 | panic recovered |

## Liveness / readiness / metrics

- `GET /health` → `200 {"status":"ok"}` (process is up).
- `GET /ready` → `200 {"ready":true,"cores":{…}}` when every configured core
  loaded and STT is up; `503` otherwise. `cores` maps
  `stt|mt|tts_magpie|tts_vits` → `"ok"` | `"not configured"` | error string.
- `GET /metrics` → Prometheus text exposition:
  `sb_requests_total{endpoint,status}`, `sb_stage_duration_seconds{stage}`
  (histogram), `sb_active_streams`, `sb_queue_depth{core}`,
  `sb_core_up{core}`, `sb_audio_seconds_total`.

## `GET /v1/capabilities`

```json
{
  "cores": { "stt": "ok", "mt": "not configured", "tts_magpie": "ok", "tts_vits": "ok" },
  "tts":   { "magpie": ["ar","de","en","es","fr","hi","it","ko","pt-BR","vi"],
             "vits":   ["ru"] },
  "stt_langs": ["uz","ru","kaa","auto"]
}
```

`tts` is computed at startup from each model's `sb_tts_languages()` and which
models actually loaded — it is the real coverage, not the static routing table.

## Batch REST

All batch endpoints are authed and queued (`SB_QUEUE_DEPTH`); request timeout
180 s.

### `POST /v1/speech-to-speech`

`multipart/form-data` with `file=<wav>` (or `audio=<wav>`), plus form fields
`source_lang` (default `auto`), `target_lang` (**required**), `voice`.
Alternatively a raw `audio/wav` body with `?source_lang=&target_lang=&voice=`.

```json
{
  "request_id": "…",
  "transcript": "…",
  "translation": "…",
  "audio": "<base64 WAV>",           // omitted if no audio was produced
  "audio_format": "wav",
  "sample_rate": 22050,
  "timings": { "stt_ms": 0, "mt_ms": 0, "tts_ms": 0, "total_ms": 0 },
  "stage": "ok"
}
```

`stage` ∈ `ok` | `empty_transcript` | `mt_unavailable` | `tts_unavailable`.
Degraded stages still return **200**: captions continue when MT/TTS are missing
(MT down → the source text passes through; TTS down → no `audio`). An empty
transcript returns 200 with `stage:"empty_transcript"` and empty fields. STT
unavailable → 503 `stt_unavailable`.

### `POST /v1/transcribe`

Same request forms as above (`target_lang` not needed). STT only.

```json
{ "request_id": "…", "transcript": "…", "timings": { "stt_ms": 0, "total_ms": 0 } }
```

### `POST /v1/translate`

```json
{ "text": "…", "source_lang": "auto", "target_lang": "uz" }
```
→ `{ "request_id": "…", "translation": "…" }`. Text is sentence-split first;
Uzbek output is transliterated to Latin for display.

### `POST /v1/speak`

```json
{ "text": "…", "lang": "ru", "voice": "", "encoding": "wav" }
```
`encoding` `"wav"` (default) returns `audio/wav` bytes with `X-Sample-Rate`;
`"base64"` returns `{ "request_id", "audio", "audio_format":"wav", "sample_rate" }`.

## WebSocket `/v1/stream`

One connection = one speaker → one target language. Meeting rooms are out of
scope. Idle timeout 60 s; at most `SB_STREAMS_MAX` concurrent streams.

**Client → server**

1. one JSON text frame:
   ```json
   { "type":"start", "source_lang":"ru", "target_lang":"uz",
     "voice":"", "sample_rate":16000, "format":"f32" }
   ```
   `format` `f32` (little-endian float32) or `s16` (little-endian int16).
2. binary PCM frames, ~250 ms each, at `sample_rate` (resampled to 16 kHz
   server-side).
3. a JSON text frame `{ "type":"stop" }`.

**Server → client** — JSON text frames, plus one binary frame immediately
after every `audio` header:

| type | fields | when |
|------|--------|------|
| `partial` | `text`, `seq` | live caption for the in-progress utterance; may be revised |
| `transcript` | `text`, `seq`, `t0_ms`, `t1_ms` | an utterance finalized (EOU) |
| `translation` | `text`, `seq` | MT completed for that sentence |
| `audio` | `seq`, `n_samples`, `sample_rate` | followed by one binary float32-LE PCM frame |
| `error` | `code`, `stage`, `seq` | per-sentence failure — the stream continues (degraded) |
| `done` | `seq` | stream finished after `stop` / disconnect |

`seq` is the sentence index. Sentences may finish MT/TTS out of order
internally; `audio` frames are emitted in `seq` order via a reorder buffer.

## Config (env, `SB_` prefix; validated at startup, process exits on invalid)

| var | default | notes |
|-----|---------|-------|
| `SB_BIND` | `127.0.0.1:8080` | host:port |
| `SB_MODELS_DIR` | `models` | models auto-discovered here if the `SB_*_MODEL` vars are unset |
| `SB_LIB_DIR` | `lib` | `libsb_*.{so,dylib}` |
| `SB_STT_MODEL` / `SB_MT_MODEL` / `SB_TTS_MAGPIE_MODEL` | (auto) | explicit model file; empty ⇒ that core is disabled if not found |
| `SB_TTS_VITS_DIR` | (auto) | dir of `vits-mms-*` voice subdirs |
| `SB_MT_CTX` | `512` | MADLAD context, [64,4096] |
| `SB_MAX_BODY_MB` | `25` | request body cap |
| `SB_MAX_AUDIO_SEC` | `120` | batch clip cap |
| `SB_QUEUE_DEPTH` | `8` | batch admission |
| `SB_STREAMS_MAX` | `4` | concurrent WS streams |
| `SB_AUTH_TOKEN` | (empty) | empty ⇒ auth off |
| `SB_LOG_LEVEL` | `info` | `debug` also logs transcript/translation text |
| `SB_LOG_FORMAT` | `json` | `json` \| `text` |
| `SB_DEVICE` | `auto` | `cpu` \| `metal` \| `auto` |
