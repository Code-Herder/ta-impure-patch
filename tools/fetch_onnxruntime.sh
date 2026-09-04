#!/usr/bin/env bash
# Fetch ONNX Runtime  for Windows x86 and install it beside the game.
#
# The Classic++ restorer (research/notes/renderers.md §2.5) runs the unditherer's
# full model inside the DLL through Microsoft's ONNX Runtime. TotalA.exe is a
# 32-bit process; 1.22.1 is the LAST version whose NuGet package carries a
# win-x86 native runtime (1.23.0 onward ship x64 and arm64 only), but its C++
# runtime needs `std::_Throw_Cpp_error`, which Wine 9's built-in msvcp140 does
# not implement (measured 2026-09-04: "unimplemented function ... aborting"),
# so 1.20.1 — the last one that runs on the built-in runtime — is pinned and
# its DLL is checked by hash. A prefix with the native VC++ 2019 runtime could
# take 1.22.1.
#
# Installs into the template gamedir (tagpu/gamedir at the main checkout, which
# tacli mirrors into every instance), or into the directory given as $1:
#   onnxruntime.dll                    the runtime (MIT — see the two files below)
#   onnxruntime_LICENSE.txt            Microsoft's MIT licence text
#   onnxruntime_ThirdPartyNotices.txt  the notices the package ships
#   full.onnx                          the restorer model, from unditherer/models
set -euo pipefail
VER=1.20.1
SHA_DLL=8d2d4aab482f11efbf80b014d9428f90def03a4d417a75e8b2487b23af670d49
ROOT=$(cd "$(dirname "$0")/.." && pwd)
COMMON=$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)
GAMEDIR=${1:-$(dirname "$COMMON")/tagpu/gamedir}
[ -d "$GAMEDIR" ] || { echo "no gamedir at $GAMEDIR" >&2; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
URL="https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/$VER/microsoft.ml.onnxruntime.$VER.nupkg"
echo "fetching $URL"
curl -sSL --max-time 600 -o "$TMP/ort.nupkg" "$URL"
unzip -o -q "$TMP/ort.nupkg" runtimes/win-x86/native/onnxruntime.dll LICENSE ThirdPartyNotices.txt -d "$TMP/x"
echo "$SHA_DLL  $TMP/x/runtimes/win-x86/native/onnxruntime.dll" | sha256sum -c -
cp "$TMP/x/runtimes/win-x86/native/onnxruntime.dll" "$GAMEDIR/onnxruntime.dll"
cp "$TMP/x/LICENSE" "$GAMEDIR/onnxruntime_LICENSE.txt"
cp "$TMP/x/ThirdPartyNotices.txt" "$GAMEDIR/onnxruntime_ThirdPartyNotices.txt"
cp "$ROOT/unditherer/models/full.onnx" "$GAMEDIR/full.onnx"
echo "installed onnxruntime $VER (win-x86) and full.onnx into $GAMEDIR"
