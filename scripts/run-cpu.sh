#!/usr/bin/env bash
# run-cpu.sh — build (if needed) and run sb-server CPU-only.
#
# Portable across every architecture Speech Bridge supports natively:
# Linux x86_64 and macOS arm64 both work here with no extra toolchain beyond
# what scripts/doctor.sh already checks for. This is the default, always-
# available path — use run-metal.sh / run-cuda.sh only when you have the
# matching hardware and want the extra throughput.
#
# Any extra arguments are forwarded to scripts/build.sh (e.g. --debug, -j N).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "==> run-cpu: building (cores + server, CPU-only)"
"$ROOT/scripts/build.sh" "$@"

export SB_LIB_DIR="$ROOT/lib"
export SB_DEVICE="${SB_DEVICE:-cpu}"
echo "==> run-cpu: starting sb-server (SB_DEVICE=$SB_DEVICE)"
exec "$ROOT/sb-server"
