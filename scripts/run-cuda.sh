#!/usr/bin/env bash
# run-cuda.sh — build (if needed) and run sb-server with the CUDA backend.
#
# Linux x86_64 only, with the CUDA toolkit (nvcc) and an NVIDIA driver
# installed (run scripts/doctor.sh to check both). Accelerates stt, mt, and
# tts_magpie (each auto-selects a non-CPU ggml device at runtime once built
# with CUDA support); tts_vits (sherpa-onnx/onnxruntime) stays CPU-only
# regardless — its GPU path needs a different, dynamically-linked
# onnxruntime build that conflicts with this project's per-core static-link
# isolation. See docs/blockers.md #16. This path is wired but unverified
# here — no NVIDIA GPU available in development, the same situation as
# run-metal.sh on Apple hardware (docs/blockers.md #4).
#
# Any extra arguments are forwarded to scripts/build.sh (e.g. --debug, -j N).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

UNAME="$(uname -sm)"
if [[ "$UNAME" != "Linux x86_64" ]]; then
  echo "run-cuda.sh: needs Linux x86_64; this host is '$UNAME'." >&2
  echo "Use scripts/run-cpu.sh (any platform) or scripts/run-metal.sh (Apple Silicon)." >&2
  exit 1
fi
if ! command -v nvcc >/dev/null 2>&1; then
  echo "run-cuda.sh: nvcc not found — install the CUDA toolkit, or run scripts/run-cpu.sh." >&2
  exit 1
fi
if ! command -v nvidia-smi >/dev/null 2>&1 || ! nvidia-smi >/dev/null 2>&1; then
  echo "run-cuda.sh: no working NVIDIA driver detected (nvidia-smi failed)." >&2
  echo "The build would still succeed, but there'd be no GPU to run on. Run scripts/run-cpu.sh instead." >&2
  exit 1
fi

echo "==> run-cuda: building (cores + server, CUDA backend)"
"$ROOT/scripts/build.sh" --cuda "$@"

export SB_LIB_DIR="$ROOT/lib"
export SB_DEVICE="cuda"
echo "==> run-cuda: starting sb-server (SB_DEVICE=cuda)"
exec "$ROOT/sb-server"
