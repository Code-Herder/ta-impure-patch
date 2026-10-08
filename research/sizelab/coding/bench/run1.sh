#!/bin/bash
C="$(cd "$(dirname "$0")/.." && pwd)"
cd $C
W() { env -u DISPLAY -u WAYLAND_DISPLAY WINEDEBUG=-all WINEPREFIX=$C/wpfx WINEDLLOVERRIDES="mscoree,mshtml=" "$@" wine bench/bench.exe battle.idxbin battle.bin $ARGS; }
for a in "lodepng 2048" "lodepng 32768" "zstd 1" "zstd 3" "zstd 5" "zstd 9" "zstd 12" "zstd 19" "zstd 1 chain" "zstd 3 chain" "zstd 5 chain" "zstd 9 chain" "zstd 19 chain" "lz4 1" "lz4hc 9" "libdeflate 6" "libdeflate 9" "libdeflate 12"; do
  ARGS="$a"; W 2>&1 | grep -v "^wine:"
done
ARGS="lodepng 32768"; W env NICE=258 2>&1 | sed 's/^/nice258 /' | grep -v "wine:"
