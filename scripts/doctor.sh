#!/usr/bin/env bash
# doctor.sh — verify the build environment for Speech Bridge.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
rc=0
say()  { printf '  %-22s %s\n' "$1" "$2"; }
fail() { printf '  %-22s %s\n' "$1" "FAIL: $2"; rc=1; }
warn() { printf '  %-22s %s\n' "$1" "WARN: $2"; }

echo "==> Speech Bridge doctor"

UNAME="$(uname -sm)"
case "$UNAME" in
  "Linux x86_64"|"Darwin arm64") say "platform" "$UNAME" ;;
  *) fail "platform" "$UNAME unsupported (need Linux x86_64 or Darwin arm64)" ;;
esac

# --- compilers ---
if command -v cc >/dev/null; then say "c compiler" "$(cc --version | head -1)"; else fail "c compiler" "not found"; fi
if command -v c++ >/dev/null; then say "c++ compiler" "$(c++ --version | head -1)"; else fail "c++ compiler" "not found"; fi

# --- cmake >= 3.20 ---
if command -v cmake >/dev/null; then
  v="$(cmake --version | head -1 | awk '{print $3}')"
  if [[ "$(printf '%s\n3.20.0\n' "$v" | sort -V | head -1)" == "3.20.0" ]]; then say "cmake" "$v"; else fail "cmake" "$v < 3.20"; fi
else
  fail "cmake" "not found"
fi

# --- zlib (uWebSockets' permessage-deflate needs it) ---
if [[ "$UNAME" == "Linux x86_64" ]]; then
  if [[ -f /usr/include/zlib.h ]] || ldconfig -p 2>/dev/null | grep -q libz.so; then
    say "zlib" "present"
  else
    fail "zlib" "not found (apt install zlib1g-dev)"
  fi
fi

# --- node (build-time only, for the frontend) ---
if command -v node >/dev/null; then say "node" "$(node --version) (build-time only)"; else warn "node" "not found — make web will use the committed web/dist/ as-is"; fi

# --- vendored trees ---
for t in parakeet.cpp llama.cpp magpie-tts.cpp sherpa-onnx; do
  if [[ -d "$ROOT/third_party/$t" ]]; then say "third_party/$t" "present"; else fail "third_party/$t" "missing (see third_party/PINNED.md)"; fi
done
for g in parakeet.cpp/third_party/ggml magpie-tts.cpp/third_party/ggml llama.cpp/ggml; do
  [[ -f "$ROOT/third_party/$g/CMakeLists.txt" ]] && say "ggml: $g" "present" || fail "ggml: $g" "missing"
done

# --- platform specifics ---
if [[ "$UNAME" == "Darwin arm64" ]]; then
  if xcode-select -p >/dev/null 2>&1; then say "xcode CLT" "$(xcode-select -p)"; else fail "xcode CLT" "run: xcode-select --install"; fi
  if xcrun --find metal >/dev/null 2>&1; then say "metal toolchain" "present (--metal builds enabled)"; else warn "metal toolchain" "absent — CPU-only; install full Xcode for --metal"; fi
  ramgb=$(( $(sysctl -n hw.memsize) / 1073741824 ))
  say "RAM" "${ramgb} GB"
  [[ "$ramgb" -lt 16 ]] && warn "RAM" "<16 GB on macOS — model loads may be tight"
fi
if [[ "$UNAME" == "Linux x86_64" ]]; then
  say "glibc" "$(ldd --version 2>/dev/null | head -1 || echo unknown)"
  ramgb=$(( $(awk '/MemTotal/{print $2}' /proc/meminfo) / 1048576 ))
  say "RAM" "${ramgb} GB"
  [[ "$ramgb" -lt 8 ]] && warn "RAM" "<8 GB on Linux — model loads may fail"
fi

echo
[[ $rc -eq 0 ]] && echo "==> doctor: OK" || echo "==> doctor: problems found"
exit $rc
