#!/usr/bin/env bash
# build.sh — single entry point for building Speech Bridge natively.
#
#   detect platform  ->  build cores via CMake (one build tree each)
#                    ->  check exported symbols
#                    ->  go build the app
#                    ->  print artifact paths
#
# cgo forbids practical cross-compilation: each platform builds natively.
#
# Flags:
#   --debug        CMAKE_BUILD_TYPE=Debug, go build with -gcflags "all=-N -l"
#   --release      CMAKE_BUILD_TYPE=Release, portable ISA baseline (see below)
#   --clean        remove build/ and lib/ and app binaries first
#   --cores-only   stop after cores + symbol check
#   --app-only     skip cores, only go build
#   --metal        build cores' ggml with the Metal backend (needs full Xcode)
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
DEBUG_GO=0
JOBS=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug)      BUILD_TYPE="Debug"; DEBUG_GO=1 ;;
    --release)    BUILD_TYPE="Release" ;;
    --clean)      DO_CLEAN=1 ;;
    --cores-only) CORES_ONLY=1; DO_APP=0 ;;
    --app-only)   DO_CORES=0 ;;
    --metal)      METAL=1 ;;
    -j)           shift; JOBS="$1" ;;
    -j*)          JOBS="${1#-j}" ;;
    *) echo "build.sh: unknown flag: $1" >&2; exit 2 ;;
  esac
  shift
done

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

LIB_DIR="$ROOT/lib"
BUILD_DIR="$ROOT/build"
CORES=(stt mt tts_magpie tts_vits)

echo "==> Speech Bridge build"
echo "    platform   : $PLATFORM ($UNAME)"
echo "    build type : $BUILD_TYPE"
echo "    jobs       : $JOBS"
echo "    metal      : $METAL"

if [[ "$DO_CLEAN" == "1" ]]; then
  echo "==> clean"
  rm -rf "$BUILD_DIR" "$LIB_DIR"
  rm -f "$ROOT/app/sb-server"
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
      "${METAL_ARG[@]}"
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

# --- app ---------------------------------------------------------------
if [[ "$DO_APP" == "1" ]]; then
  if [[ -f "$ROOT/go.mod" ]]; then
    echo "==> go build ./app/cmd/sb-server"
    GO_FLAGS=()
    [[ "$DEBUG_GO" == "1" ]] && GO_FLAGS=(-gcflags "all=-N -l")
    VERSION="$(cat "$ROOT/VERSION" 2>/dev/null || echo dev)"
    ( cd "$ROOT" && CGO_ENABLED=1 go build ${GO_FLAGS[@]+"${GO_FLAGS[@]}"} \
        -ldflags "-X main.version=$VERSION" \
        -o "$ROOT/app/sb-server" ./app/cmd/sb-server )
    echo "    -> $ROOT/app/sb-server"
  else
    echo "==> app: skipped (go.mod not present)"
  fi
fi

echo
echo "==> artifacts"
ls -la "$LIB_DIR" 2>/dev/null || true
[[ -f "$ROOT/app/sb-server" ]] && ls -la "$ROOT/app/sb-server"
echo "==> done"
