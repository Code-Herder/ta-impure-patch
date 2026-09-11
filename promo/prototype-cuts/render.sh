#!/usr/bin/env bash
# PROTOTYPE — throwaway. One command: build the five cuts and a page to compare them.
#
#   promo/prototype-cuts/render.sh [outdir]
#
# Renders at 960x540 because this is for judging PACING, not pixels — five cuts at
# 1080p is ~6 minutes of rendering to answer a question that half-resolution answers
# just as well. Output (json, mp4, index.html) all lands in one throwaway directory.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:-${TMPDIR:-/tmp}/tacli-promo-cuts}"
PY="$(git -C "$REPO" rev-parse --git-common-dir)/../.venv-undither/bin/python"

[ -x "$PY" ] || { echo "no venv python at $PY" >&2; exit 1; }

echo "== generating variants"
"$PY" "$REPO/promo/prototype-cuts/make-variants.py" "$OUT"

echo
echo "== rendering (960x540, judging pacing not pixels)"
for j in "$OUT"/*.json; do
  id="$(basename "$j" .json)"
  [ "$id" = manifest ] && continue
  printf '  %-14s ' "$id"
  start=$SECONDS
  "$PY" "$REPO/tools/tamontage" render "$j" -o "$OUT/$id.mp4" \
      --width 960 --height 540 --crf 28 --preset veryfast >/dev/null
  # An encode is unverified until it decodes end to end (ta-video-montage rule 7).
  if ffmpeg -v error -i "$OUT/$id.mp4" -f null - 2>/dev/null; then
    echo "ok  $((SECONDS - start))s  $(du -h "$OUT/$id.mp4" | cut -f1)"
  else
    echo "DECODE FAILED"; exit 1
  fi
done

echo
echo "open:  $OUT/index.html"
