#!/usr/bin/env bash
# test-e2e.sh — real cores + real models, black-box over the actual HTTP API.
# Skips cleanly (exit 0) when models or the built server aren't present, so
# `make test-e2e` is safe to run before `make fetch-models`.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BIND="127.0.0.1:8099"
FIXTURE="$ROOT/third_party/parakeet.cpp/tests/fixtures/speech.wav"

skip() { echo "test-e2e: SKIP — $1"; exit 0; }

[[ -x "$ROOT/sb-server" ]] || skip "sb-server not built (run ./scripts/build.sh)"
[[ -d "$ROOT/lib" ]] || skip "lib/ not built"
compgen -G "$ROOT/models/stt/*.gguf" >/dev/null 2>&1 || skip "no STT model under models/ (run scripts/fetch-models.sh)"
[[ -f "$FIXTURE" ]] || skip "fixture WAV not found: $FIXTURE"
command -v curl >/dev/null || skip "curl required"

SB_LIB_DIR="$ROOT/lib" SB_MODELS_DIR="$ROOT/models" SB_WEB_DIST_DIR="$ROOT/web/dist" \
  SB_BIND="$BIND" SB_LOG_LEVEL=warn "$ROOT/sb-server" >/tmp/sb-test-e2e.log 2>&1 &
SERVER_PID=$!
cleanup() { kill "$SERVER_PID" >/dev/null 2>&1 || true; wait "$SERVER_PID" 2>/dev/null || true; }
trap cleanup EXIT

echo "test-e2e: waiting for /ready ..."
ready=0
for _ in $(seq 1 60); do
  if curl -fs "http://$BIND/ready" >/dev/null 2>&1; then ready=1; break; fi
  sleep 1
done
if [[ "$ready" != "1" ]]; then
  echo "test-e2e: FAIL — server never became ready"
  tail -40 /tmp/sb-test-e2e.log
  exit 1
fi

echo "test-e2e: POST /v1/speech-to-speech (en -> ru)"
out="$(curl -fs -X POST "http://$BIND/v1/speech-to-speech" \
  -F file=@"$FIXTURE" -F source_lang=en -F target_lang=ru)"
if [[ -z "$out" ]]; then
  echo "test-e2e: FAIL — empty response"
  exit 1
fi

stage="$(echo "$out" | grep -o '"stage":"[^"]*"' | head -1)"
transcript_len="$(echo "$out" | grep -o '"transcript":"[^"]*"' | wc -c)"
echo "test-e2e: $stage"

if [[ "$stage" != '"stage":"ok"' ]]; then
  echo "test-e2e: FAIL — unexpected stage"
  echo "$out"
  exit 1
fi
if [[ "$transcript_len" -le 20 ]]; then
  echo "test-e2e: FAIL — transcript looks empty"
  echo "$out"
  exit 1
fi

echo "test-e2e: PASS"
