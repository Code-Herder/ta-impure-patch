#!/bin/bash
# run.sh <label> <wine-binary> <prefix-dir>
#
# Drives dptest.exe through the three cases that matter and leaves the logs in
# out-<label>/.  Uses a throwaway prefix so nothing touches tagpu/instances/.
#
#   ./run.sh wine9  /usr/bin/wine /tmp/pfx9
#   ./run.sh wine11 "$HOME/.steam/steam/steamapps/common/Proton - Experimental/files/bin/wine" /tmp/pfx11
set -u
LABEL="$1"; WINE="$2"; PFX="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
export WINEPREFIX="$PFX"
export WINEDEBUG="-all,+dplay,+dplayx,+dpwsockx"
export DISPLAY="${DISPLAY:-:0}"
EXE="$HERE/dptest.exe"
OUT="$HERE/out-$LABEL"
mkdir -p "$OUT" "$PFX"

[ -f "$EXE" ] || { echo "build first: make -C $HERE"; exit 1; }

echo "### $LABEL : $("$WINE" --version 2>&1 | head -1)"
"$WINE" wineboot -u >"$OUT/boot.log" 2>&1
sleep 2

# 1. enumerate with nobody hosting -- distinguishes "no sessions" from "not implemented"
"$WINE" "$EXE" enum 127.0.0.1 >"$OUT/enum-alone.log" 2>"$OUT/enum-alone.err"

# 2. host in the background, then join it
"$WINE" "$EXE" host 127.0.0.1 >"$OUT/host.log" 2>"$OUT/host.err" &
HOSTPID=$!
sleep 6
"$WINE" "$EXE" join 127.0.0.1 >"$OUT/join.log" 2>"$OUT/join.err"
sleep 4
kill $HOSTPID 2>/dev/null; wait $HOSTPID 2>/dev/null
"$WINE" wineboot -e >/dev/null 2>&1

echo "--- $LABEL results ---"
grep -hE "EnumSessions |Open\(DPOPEN|CreatePlayer |GAME MSG" "$OUT"/*.log
echo "### $LABEL done"
