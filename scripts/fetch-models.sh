#!/usr/bin/env bash
# fetch-models.sh — download model weights listed in models/MANIFEST.md and
# verify each against its sha256. Architecture-independent (same files on
# Linux x86_64 and macOS arm64). No Python.
#
# Idempotent: an entry whose target already exists and matches its sha256 is
# skipped. Partial downloads are resumed (curl -C -).
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

# --- parse the SB-MANIFEST block (portable to bash 3.2 — no mapfile) ----
ENTRIES=()
while IFS= read -r _l; do
  ENTRIES+=("$_l")
done < <(awk '
  /<!-- SB-MANIFEST-BEGIN -->/ {inb=1; next}
  /<!-- SB-MANIFEST-END -->/   {inb=0}
  inb && $0 !~ /^```/ && NF {print}
' "$MANIFEST")

[[ ${#ENTRIES[@]} -gt 0 ]] || { echo "fetch-models: no entries in manifest block" >&2; exit 1; }

echo "==> fetch-models"
echo "    dest     : $DEST"
echo "    entries  : ${#ENTRIES[@]}"
echo

mkdir -p "$DEST"
fail=0
declare -a SUMMARY

verify() { # path expected_sha  -> 0 ok / 1 mismatch
  local got; got="$(sha256 "$1")"
  [[ "$got" == "$2" ]]
}

for line in "${ENTRIES[@]}"; do
  IFS=$'\t' read -r kind dest sha bytes url <<<"$line"
  [[ -n "${url:-}" ]] || { echo "  ! malformed line: $line" >&2; fail=1; continue; }
  target="$DEST/$dest"

  case "$kind" in
    gguf)
      if [[ -f "$target" ]] && verify "$target" "$sha"; then
        echo "  = $dest (present, sha ok)"
        SUMMARY+=("ok    $dest")
        continue
      fi
      mkdir -p "$(dirname "$target")"
      echo "  ↓ $dest  ($(( bytes / 1048576 )) MiB)"
      curl -fL --retry 3 --retry-delay 2 -C - -o "$target.part" "$url"
      mv "$target.part" "$target"
      if verify "$target" "$sha"; then
        echo "    sha ok"
        SUMMARY+=("ok    $dest")
      else
        echo "    SHA MISMATCH: got $(sha256 "$target"), want $sha" >&2
        SUMMARY+=("BAD   $dest")
        fail=1
      fi
      ;;
    targz)
      marker="$target/.sb-sha256"
      if [[ -f "$marker" ]] && [[ "$(cat "$marker")" == "$sha" ]]; then
        echo "  = $dest/ (present, sha ok)"
        SUMMARY+=("ok    $dest/")
        continue
      fi
      tmp="$DEST/.$(basename "$dest").tar.bz2"
      echo "  ↓ $dest/  ($(( bytes / 1048576 )) MiB, archive)"
      curl -fL --retry 3 --retry-delay 2 -C - -o "$tmp" "$url"
      if ! verify "$tmp" "$sha"; then
        echo "    SHA MISMATCH: got $(sha256 "$tmp"), want $sha" >&2
        rm -f "$tmp"; SUMMARY+=("BAD   $dest/"); fail=1; continue
      fi
      rm -rf "$target"
      mkdir -p "$(dirname "$target")"
      # archives contain a single top-level dir; flatten it into $dest/
      tmpd="$(mktemp -d "$DEST/.extract.XXXXXX")"
      tar xjf "$tmp" -C "$tmpd"
      inner="$(find "$tmpd" -mindepth 1 -maxdepth 1 -type d | head -1)"
      mv "$inner" "$target"
      rm -rf "$tmpd" "$tmp"
      printf '%s\n' "$sha" > "$marker"
      echo "    sha ok, extracted"
      SUMMARY+=("ok    $dest/")
      ;;
    *)
      echo "  ! unknown kind '$kind' for $dest" >&2; fail=1 ;;
  esac
done

echo
echo "==> summary"
printf '    %s\n' "${SUMMARY[@]}"
echo
echo "    Uzbek/Karakalpak VITS voices are NOT fetched (no pre-built package)."
echo "    See the 'Deferred' section of models/MANIFEST.md and docs/blockers.md."

exit $fail
