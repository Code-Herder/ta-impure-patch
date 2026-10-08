#!/bin/bash
C="$(cd "$(dirname "$0")/.." && pwd)"
cd $C
for tag in H3D4T1L2-N0 H3D4T1L2-N300; do
for a in "lodepng 2048" "zstd 1" "zstd 3" "zstd 5" "zstd 9" "zstd 19" "zstd 3 chain" "zstd 5 chain" "zstd 9 chain"; do
  echo -n "$tag "; env -u DISPLAY -u WAYLAND_DISPLAY WINEDEBUG=-all WINEPREFIX=$C/wpfx WINEDLLOVERRIDES="mscoree,mshtml=" wine bench/bench.exe re-$tag.idxbin re-$tag.bin $a 2>&1 | grep -v "^wine:"
done
done
