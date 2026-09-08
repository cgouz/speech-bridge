#!/usr/bin/env bash
# check-symbols.sh — every exported symbol of every lib/libsb_*.{so,dylib} must
# match ^_?sb_ . A leaked ggml_* (or any other) symbol fails the build: it means
# an engine's ggml is not fully contained and could collide with another core's.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB_DIR="$ROOT/lib"

case "$(uname -s)" in
  Linux)  EXT="so";    list_syms() { nm -D --defined-only "$1" | awk '{print $3}'; } ;;
  Darwin) EXT="dylib"; list_syms() { nm -gU "$1" | awk '{print $3}'; } ;;
  *) echo "check-symbols.sh: unsupported OS" >&2; exit 1 ;;
esac

shopt -s nullglob
libs=("$LIB_DIR"/libsb_*."$EXT")
shopt -u nullglob

if [[ ${#libs[@]} -eq 0 ]]; then
  echo "check-symbols.sh: no libs in $LIB_DIR — build cores first" >&2
  exit 1
fi

rc=0
for lib in "${libs[@]}"; do
  bad=$(list_syms "$lib" | grep -vE '^_?sb_' | grep -vE '^$' || true)
  # macOS: ignore the standard dynamic-lookup / indirect entries nm may print
  bad=$(echo "$bad" | grep -vE '^(dyld_stub_binder|__mh_dylib_header|radr://[0-9]+)$' || true)
  n=$(echo -n "$bad" | grep -c . || true)
  if [[ "$n" -ne 0 ]]; then
    echo "FAIL $(basename "$lib"): $n non-sb_ exported symbol(s):"
    echo "$bad" | sed 's/^/    /' | head -40
    rc=1
  else
    echo "OK   $(basename "$lib"): exports only sb_*"
  fi
done

exit $rc
