# Operations

Deploying and running `sb-server`. Expanded in milestone 8.

- systemd unit: `deploy/systemd/speechbridge.service` (MemoryMax=8G,
  Restart=on-failure).
- Docker: `deploy/docker/Dockerfile` (multi-stage; models mounted via volume,
  never baked in).
- Limits: body 25 MB; batch audio <=120 s (else 413); queue full -> 429 +
  Retry-After; batch timeout 180 s; WS idle timeout 60 s.
- Graceful shutdown: SIGTERM -> stop accepting -> drain 30 s -> free contexts
  -> free models -> exit.
- Metrics: `sb_requests_total`, `sb_stage_duration_seconds`, `sb_active_streams`,
  `sb_queue_depth`, `sb_core_up`, `sb_audio_seconds_total`.
