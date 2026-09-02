#!/bin/bash
# dpinstall.sh -- install native Microsoft DirectPlay into a wine prefix.
#
# Wine implements the client half of DirectPlay TCP/IP and not the host half
# (dpwsockx: "session creation is not yet supported", still true on master), so
# no wine process can create a session. Dropping Microsoft's own dplayx/dpwsockx
# in front of wine's builtins fixes that -- verified end to end with
# tools/dptest on wine 9.0, in both win32 and win64 prefixes.
#
#   ./dpinstall.sh <wineprefix> [src]
#
# src defaults to ~/.local/share/ta-directplay (see its PROVENANCE.txt for where
# the files came from and how their Microsoft signature was verified).
set -eu

PFX="${1:?usage: dpinstall.sh <wineprefix> [src]}"
SRC="${2:-$HOME/.local/share/ta-directplay}"

FILES="dplayx.dll dpwsockx.dll dpmodemx.dll dpnet.dll dpnhpast.dll dpnhupnp.dll dplaysvr.exe dpnsvr.exe"
OVERRIDES="dplayx,dpmodemx,dpnet,dpnhpast,dpnhupnp,dpwsockx,dplaysvr.exe,dpnsvr.exe=n"

[ -d "$PFX/drive_c" ] || { echo "not a wine prefix: $PFX" >&2; exit 1; }
for f in $FILES; do
    [ -f "$SRC/$f" ] || { echo "missing $SRC/$f" >&2; exit 1; }
done

# 32-bit DLLs go in syswow64 on a 64-bit prefix, system32 on a 32-bit one.
if [ -d "$PFX/drive_c/windows/syswow64" ]; then
    DEST="$PFX/drive_c/windows/syswow64"; ARCH="win64"
else
    DEST="$PFX/drive_c/windows/system32"; ARCH="win32"
fi

for f in $FILES; do cp -f "$SRC/$f" "$DEST/$f"; done
# On a 64-bit prefix the two servers must also shadow the 64-bit builtins,
# or dplayx launches wine's stub dplaysvr and Open() hangs.
if [ "$ARCH" = win64 ]; then
    cp -f "$SRC/dplaysvr.exe" "$SRC/dpnsvr.exe" "$PFX/drive_c/windows/system32/"
fi

echo "installed native DirectPlay into $DEST  ($ARCH prefix)"
echo
echo "launch with:"
echo "  WINEDLLOVERRIDES=\"$OVERRIDES\""
echo "  (append to any existing overrides, e.g. ddraw=n,b)"
echo
echo "IMPORTANT: dplaysvr.exe owns UDP 47624 and outlives the game. A stale one"
echo "from another prefix makes the next host fail with DPERR_GENERIC. Before"
echo "hosting:  pkill -x dplaysvr.exe"
