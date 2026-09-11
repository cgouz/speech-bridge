#!/usr/bin/env bash
# bench.sh — measure the latency budget against a running sb-server and print a
# table. Runs scripts/bench.mjs (Node — a build-time-only dependency already
# used for the frontend). Requires models loaded on the server.
#
#   SB_BENCH_ADDR   server address       (default 127.0.0.1:8080)
#   SB_BENCH_WAV    test clip            (default: parakeet fixture, English)
#   SB_BENCH_SRC / SB_BENCH_DST   languages (default en -> ru)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ADDR="${SB_BENCH_ADDR:-127.0.0.1:8080}"
WAV="${SB_BENCH_WAV:-$ROOT/third_party/parakeet.cpp/tests/fixtures/speech.wav}"
SRC="${SB_BENCH_SRC:-en}"
DST="${SB_BENCH_DST:-ru}"

[[ -f "$WAV" ]] || { echo "bench: clip not found: $WAV" >&2; exit 1; }
command -v node >/dev/null || { echo "bench: node is required (build-time-only dependency)" >&2; exit 1; }
if ! curl -sf "http://$ADDR/health" >/dev/null; then
  echo "bench: no server at $ADDR (start it with: make run)" >&2
  exit 1
fi

exec node "$ROOT/scripts/bench.mjs" --addr "$ADDR" --wav "$WAV" --src "$SRC" --dst "$DST"
