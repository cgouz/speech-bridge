# Speech Bridge frontend

Vue 3 + Vite + TypeScript SPA for `sb-server`. It is a build-time-only
dependency: the built output (`../dist/`) is served straight off disk by the
C++ server (`server/src/httpapi/server.cpp`, `SB_WEB_DIST_DIR` — defaults to
`web/dist` next to the binary), so the shipped artifact stays
`sb-server` + 4 `.so`/`.dylib` cores + models + this directory — no Node.js at
runtime.

## Develop

```sh
npm install
npm run dev            # http://localhost:5173, proxies /v1, /health, /ready,
                        # /metrics (incl. the WS upgrade) to :8080
```

Run `sb-server` separately (`make run` from the repo root) so the proxy has
something to talk to.

## Build

```sh
npm run build           # -> ../dist/  (served from disk by sb-server)
```

Or from the repo root: `make web`, or just `./scripts/build.sh` (runs it
automatically when `npm` is on `PATH`, otherwise skips with a message and the
previously-committed `../dist/` is used as-is).

`../dist/` is **committed to the repo** so a host with no Node.js installed
still ships a working frontend — Node is a build-time convenience, not a
runtime dependency of the deployed server. If you change frontend source,
rebuild (`npm run build` or `make web`) and commit the refreshed `dist/`
alongside it, the same way you'd commit any other generated artifact this
repo tracks (e.g. vendored third-party sources).

## Structure

- `src/api/` — typed REST client (`client.ts`) and the `/v1/stream` WebSocket
  client (`stream.ts`), typed against `docs/api.md` /
  `server/src/httpapi/session.h`.
- `src/audio/` — mic capture via an `AudioWorklet` (`pcm-worklet.js`,
  `capture.ts`) and gapless playback (`playback.ts`).
- `src/stores/` — Pinia stores: `settings` (persisted to `localStorage`:
  token, languages, voice) and `health` (polls `/ready` + `/v1/capabilities`).
- `src/components/` — `SetupPanel`, `LiveSession` (the WS meeting-interpreter
  view), `BatchPanel` (REST endpoints), `CapabilitiesView` (observability),
  `MicLoopback` (pure mic→speaker passthrough, no server involved — for
  checking hardware before a live session).

No component framework or CSS library — hand-written CSS, light/dark via
`prefers-color-scheme`. No CDN dependencies; everything is bundled so the app
works fully offline.
