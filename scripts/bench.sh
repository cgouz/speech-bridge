#!/usr/bin/env bash
# bench.sh — measure the latency budget against a running sb-server and print a
# table. Builds and runs app/cmd/sb-bench. Requires models loaded on the server.
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
if ! curl -sf "http://$ADDR/health" >/dev/null; then
  echo "bench: no server at $ADDR (start it with: make run)" >&2
  exit 1
fi

go build -o "$ROOT/build/sb-bench" ./app/cmd/sb-bench
exec "$ROOT/build/sb-bench" -addr "$ADDR" -wav "$WAV" -src "$SRC" -dst "$DST"
