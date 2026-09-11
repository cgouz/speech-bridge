#!/usr/bin/env bash
# build.sh — single entry point for building Speech Bridge natively.
#
#   detect platform  ->  build cores via CMake (one build tree each)
#                    ->  check exported symbols
#                    ->  build the frontend (if node is available)
#                    ->  build the C++ server via CMake
#                    ->  print artifact paths
#
# Every native build (cores + server) is CMake, built natively per platform.
#
# Flags:
#   --debug        CMAKE_BUILD_TYPE=Debug for cores and the server
#   --release      CMAKE_BUILD_TYPE=Release, portable ISA baseline (see below)
#   --clean        remove build/ and lib/ and app binaries first
#   --cores-only   stop after cores + symbol check
#   --app-only     skip cores, only build the server (+ frontend)
#   --metal        build cores' ggml with the Metal backend (needs full Xcode; macOS only)
#   --cuda         build cores' ggml with the CUDA backend (needs CUDA toolkit; Linux only)
#                  covers stt/mt/tts_magpie; tts_vits stays CPU-only either way
#                  (see docs/blockers.md #16)
#   -j N           parallelism (default: CPU count)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_TYPE="Release"
DO_CORES=1
DO_APP=1
DO_CLEAN=0
CORES_ONLY=0
METAL=0
CUDA=0
JOBS=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug)      BUILD_TYPE="Debug" ;;
    --release)    BUILD_TYPE="Release" ;;
    --clean)      DO_CLEAN=1 ;;
    --cores-only) CORES_ONLY=1; DO_APP=0 ;;
    --app-only)   DO_CORES=0 ;;
    --metal)      METAL=1 ;;
    --cuda)       CUDA=1 ;;
    -j)           shift; JOBS="$1" ;;
    -j*)          JOBS="${1#-j}" ;;
    *) echo "build.sh: unknown flag: $1" >&2; exit 2 ;;
  esac
  shift
done

if [[ "$METAL" == "1" && "$CUDA" == "1" ]]; then
  echo "build.sh: --metal and --cuda are mutually exclusive" >&2
  exit 2
fi

# --- platform detection ---------------------------------------------------
UNAME="$(uname -sm)"
case "$UNAME" in
  "Linux x86_64")
    PLATFORM="linux"; LIBEXT="so"
    : "${JOBS:=$(nproc)}"
    CMAKE_PLATFORM_ARGS=()
    ;;
  "Darwin arm64")
    PLATFORM="macos"; LIBEXT="dylib"
    : "${JOBS:=$(sysctl -n hw.ncpu)}"
    CMAKE_PLATFORM_ARGS=(-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0)
    ;;
  *)
    echo "build.sh: unsupported platform '$UNAME'." >&2
    echo "Speech Bridge builds natively on: Linux x86_64, macOS arm64." >&2
    exit 1
    ;;
esac

if [[ "$BUILD_TYPE" == "Release" && "$PLATFORM" == "linux" ]]; then
  # Portable server baseline: no -march=native. AVX2 + FMA run on any modern x86_64.
  CMAKE_PLATFORM_ARGS+=(-DGGML_NATIVE=OFF -DGGML_AVX2=ON -DGGML_FMA=ON)
else
  CMAKE_PLATFORM_ARGS+=(-DGGML_NATIVE=ON)
fi

METAL_ARG=(-DSB_METAL=OFF)
if [[ "$METAL" == "1" ]]; then
  [[ "$PLATFORM" == "macos" ]] || { echo "build.sh: --metal is macOS only" >&2; exit 2; }
  METAL_ARG=(-DSB_METAL=ON)
fi

CUDA_ARG=(-DSB_CUDA=OFF)
if [[ "$CUDA" == "1" ]]; then
  [[ "$PLATFORM" == "linux" ]] || { echo "build.sh: --cuda is Linux only" >&2; exit 2; }
  command -v nvcc >/dev/null 2>&1 || echo "build.sh: warning: nvcc not found on PATH — the CUDA build will likely fail" >&2
  CUDA_ARG=(-DSB_CUDA=ON)
fi

LIB_DIR="$ROOT/lib"
BUILD_DIR="$ROOT/build"
CORES=(stt mt tts_magpie tts_vits)

echo "==> Speech Bridge build"
echo "    platform   : $PLATFORM ($UNAME)"
echo "    build type : $BUILD_TYPE"
echo "    jobs       : $JOBS"
echo "    metal      : $METAL"
echo "    cuda       : $CUDA"

if [[ "$DO_CLEAN" == "1" ]]; then
  echo "==> clean"
  rm -rf "$BUILD_DIR" "$LIB_DIR" "$ROOT/server/build"
  rm -f "$ROOT/sb-server"
fi

mkdir -p "$LIB_DIR"

# --- cores --------------------------------------------------------------
if [[ "$DO_CORES" == "1" ]]; then
  for core in "${CORES[@]}"; do
    src="$ROOT/cores/$core"
    bld="$BUILD_DIR/$core"
    [[ -f "$src/CMakeLists.txt" ]] || { echo "build.sh: no CMakeLists for core '$core'" >&2; exit 1; }
    echo "==> core: $core"
    cmake -S "$src" -B "$bld" \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      "${CMAKE_PLATFORM_ARGS[@]}" \
      "${METAL_ARG[@]}" \
      "${CUDA_ARG[@]}"
    cmake --build "$bld" -j "$JOBS"

    found=""
    for cand in "$bld"/libsb_*."$LIBEXT" "$bld"/*/libsb_*."$LIBEXT"; do
      [[ -f "$cand" ]] && found="$cand"
    done
    [[ -n "$found" ]] || { echo "build.sh: core '$core' produced no libsb_*.$LIBEXT" >&2; exit 1; }
    cp -f "$found" "$LIB_DIR/"
    echo "    -> $LIB_DIR/$(basename "$found")"
  done

  echo "==> check exported symbols"
  "$ROOT/scripts/check-symbols.sh"

  echo "==> one-process dlopen smoke test"
  cmake -S "$ROOT/cores/smoke" -B "$BUILD_DIR/smoke" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" >/dev/null
  cmake --build "$BUILD_DIR/smoke" -j "$JOBS" >/dev/null
  "$BUILD_DIR/smoke/sb_smoke" "$LIB_DIR"
fi

if [[ "$CORES_ONLY" == "1" ]]; then
  echo "==> done (cores only)"
  ls -la "$LIB_DIR"
  exit 0
fi

# --- frontend ------------------------------------------------------------
# Rebuilds web/dist/ from web/frontend/ when node is available. Never fatal:
# web/dist/ is committed (the server serves it straight off disk — see
# server/src/main.cpp — so a node-less host just ships whatever dist/ is
# already checked in).
if [[ "$DO_APP" == "1" && -f "$ROOT/web/frontend/package.json" ]]; then
  if command -v npm >/dev/null 2>&1; then
    echo "==> web: npm ci && npm run build"
    ( cd "$ROOT/web/frontend" && npm ci && npm run build )
  else
    echo "==> web: skipped (npm not found) — using committed web/dist/"
  fi
fi

# --- server --------------------------------------------------------------
if [[ "$DO_APP" == "1" ]]; then
  echo "==> server: cmake configure + build"
  cmake -S "$ROOT/server" -B "$ROOT/server/build" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
  cmake --build "$ROOT/server/build" -j "$JOBS" --target sb-server
  cp -f "$ROOT/server/build/sb-server" "$ROOT/sb-server"
  echo "    -> $ROOT/sb-server"
fi

echo
echo "==> artifacts"
ls -la "$LIB_DIR" 2>/dev/null || true
[[ -f "$ROOT/sb-server" ]] && ls -la "$ROOT/sb-server"
echo "==> done"
