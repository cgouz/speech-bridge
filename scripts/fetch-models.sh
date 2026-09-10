#!/usr/bin/env bash
# fetch-models.sh — download model weights listed in models/MANIFEST.md and
# verify each against its sha256. Architecture-independent (same files on
# Linux x86_64 and macOS arm64). No Python.
#
# Idempotent: an entry whose target already exists and matches its sha256 is
# skipped. Partial downloads resume (curl -C -).
#
# Env:
#   SB_MODELS_DIR   destination root (default: <repo>/models)
#   SB_MANIFEST     manifest path    (default: <repo>/models/MANIFEST.md)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="${SB_MANIFEST:-$ROOT/models/MANIFEST.md}"
DEST="${SB_MODELS_DIR:-$ROOT/models}"

[[ -f "$MANIFEST" ]] || { echo "fetch-models: missing $MANIFEST" >&2; exit 1; }
command -v curl >/dev/null || { echo "fetch-models: curl required" >&2; exit 1; }

if command -v sha256sum >/dev/null; then
  sha256() { sha256sum "$1" | awk '{print $1}'; }
elif command -v shasum >/dev/null; then
  sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
else
  echo "fetch-models: need sha256sum or shasum" >&2; exit 1
fi

# parse the SB-MANIFEST block (portable to bash 3.2 — no mapfile)
ENTRIES=()
while IFS= read -r _l; do ENTRIES+=("$_l"); done < <(awk '
  /<!-- SB-MANIFEST-BEGIN -->/ {inb=1; next}
  /<!-- SB-MANIFEST-END -->/   {inb=0}
  inb && $0 !~ /^```/ && NF {print}
' "$MANIFEST")
[[ ${#ENTRIES[@]} -gt 0 ]] || { echo "fetch-models: no entries in manifest block" >&2; exit 1; }

echo "==> fetch-models   dest=$DEST   entries=${#ENTRIES[@]}"
echo
mkdir -p "$DEST"
fail=0
SUMMARY=()

for line in "${ENTRIES[@]}"; do
  IFS=$'\t' read -r kind dest sha bytes url <<<"$line"
  [[ "$kind" == "file" && -n "${url:-}" ]] || { echo "  ! malformed: $line" >&2; fail=1; continue; }
  target="$DEST/$dest"

  if [[ -f "$target" ]] && [[ "$(sha256 "$target")" == "$sha" ]]; then
    echo "  = $dest (present, sha ok)"
    SUMMARY+=("ok    $dest"); continue
  fi

  mkdir -p "$(dirname "$target")"
  echo "  ↓ $dest  ($(( bytes / 1048576 )) MiB)"
  if ! curl -fL --retry 5 --retry-delay 3 --retry-all-errors -C - -o "$target.part" "$url"; then
    echo "    download failed (rerun to resume)" >&2
    SUMMARY+=("FAIL  $dest"); fail=1; continue
  fi
  mv "$target.part" "$target"
  if [[ "$(sha256 "$target")" == "$sha" ]]; then
    echo "    sha ok"
    SUMMARY+=("ok    $dest")
  else
    echo "    SHA MISMATCH: got $(sha256 "$target"), want $sha" >&2
    SUMMARY+=("BAD   $dest"); fail=1
  fi
done

echo
echo "==> summary"
printf '    %s\n' "${SUMMARY[@]}"
echo
echo "    Uzbek/Karakalpak VITS voices are NOT fetched (no pre-built package)."
echo "    See models/MANIFEST.md 'Deferred' section and docs/blockers.md #6."
exit $fail
