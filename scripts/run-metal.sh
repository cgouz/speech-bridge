#!/usr/bin/env bash
# run-metal.sh — build (if needed) and run sb-server with the Metal backend.
#
# macOS arm64 (Apple Silicon) only, and only on a host with full Xcode (the
# `metal`/`metallib` shader compiler — Command Line Tools alone don't ship
# it; run scripts/doctor.sh to check). Accelerates stt, mt, and tts_magpie
# (each auto-selects a non-CPU ggml device at runtime once built with Metal
# support); tts_vits (sherpa-onnx/onnxruntime) stays CPU-only regardless —
# see docs/blockers.md #16. Status of this path: wired but unverified here —
# see docs/blockers.md #4 (no full-Xcode machine available in development).
#
# Any extra arguments are forwarded to scripts/build.sh (e.g. --debug, -j N).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

UNAME="$(uname -sm)"
if [[ "$UNAME" != "Darwin arm64" ]]; then
  echo "run-metal.sh: needs macOS arm64 (Apple Silicon); this host is '$UNAME'." >&2
  echo "Use scripts/run-cpu.sh (any platform) or scripts/run-cuda.sh (Linux + NVIDIA)." >&2
  exit 1
fi
if ! xcrun --find metal >/dev/null 2>&1; then
  echo "run-metal.sh: no Metal shader compiler found (Command Line Tools alone don't ship it)." >&2
  echo "Install full Xcode, or run scripts/run-cpu.sh for a CPU-only build." >&2
  exit 1
fi

echo "==> run-metal: building (cores + server, Metal backend)"
"$ROOT/scripts/build.sh" --metal "$@"

export SB_LIB_DIR="$ROOT/lib"
export SB_DEVICE="metal"
echo "==> run-metal: starting sb-server (SB_DEVICE=metal)"
exec "$ROOT/sb-server"
