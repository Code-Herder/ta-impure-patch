#!/usr/bin/env bash
# Fetch the Classic++ restorer's inference stack for Windows x86 and install it
# beside the game.
#
# The restorer (research/notes/renderers.md §2.5) runs the unditherer's full
# model inside the DLL through Microsoft's ONNX Runtime. TotalA.exe is a 32-bit
# process; 1.22.1 is the LAST version whose NuGet package carries a win-x86
# native runtime (1.23.0 onward ship x64 and arm64 only), but its C++ runtime
# needs `std::_Throw_Cpp_error`, which Wine 9's built-in msvcp140 does not
# implement (measured 2026-09-04: "unimplemented function ... aborting"), so
# 1.20.1 — the last one that runs on the built-in runtime — is pinned and every
# file is checked by hash. A prefix with the native VC++ 2019 runtime could take
# 1.22.1.
#
# The DirectML flavour of that package is installed rather than the CPU-only
# one: same version, and a superset — the same DLL serves the CPU provider at
# the same speed when the GPU side is unavailable. On the GPU it is 34-41x the
# four-thread CPU rate and agrees with it to 0.0001 of an 8-bit level.
#
# DirectML is a Direct3D 12 stack, and Wine 9's built-in vkd3d cannot host it:
# ID3D12Device5::EnumerateMetaCommands is a stub, so the provider append fails
# with E_NOTIMPL, and CheckFeatureSupport answers shader model 5.1 to DirectML's
# 6.6 ask. vkd3d-proton does host it, so its 32-bit d3d12.dll/d3d12core.dll are
# installed too and tacli's WINEDLLOVERRIDES prefers them ("d3d12,d3d12core=n,b"
# — an instance without the files keeps the built-in, and the restorer then
# falls back to the CPU provider). vkd3d-proton is LGPL-2.1; its licence text is
# in its source tree, not the release tarball, so this script does not copy one.
# On real Windows none of this arises: the system D3D12 hosts DirectML directly.
#
# Installs into the template gamedir (tagpu/gamedir at the main checkout, which
# tacli mirrors into every instance), or into the directory given as $1:
#   onnxruntime.dll                    the runtime, DirectML build (MIT)
#   onnxruntime_LICENSE.txt            Microsoft's MIT licence text
#   onnxruntime_ThirdPartyNotices.txt  the notices the package ships
#   DirectML.dll                       the DirectML runtime it loads
#   DirectML_LICENSE.txt               Microsoft's DirectML licence terms
#   DirectML_ThirdPartyNotices.txt     the notices that package ships
#   d3d12.dll, d3d12core.dll           vkd3d-proton (LGPL-2.1), Wine only
#   full.onnx                          the restorer model, from unditherer/models
set -euo pipefail
VER=1.20.1
DML_VER=1.15.2          # the version Microsoft.ML.OnnxRuntime.DirectML.nuspec depends on
VKD3D_VER=3.0.1
SHA_ORT=5193b196c09886da143bcac2405eaa1e02170c1e11da0a06fffc94da29327de2
SHA_DML=78caa7a7b07284fe916a3dcb364d8626e40cd58a21710a18fcb7794269b00a75
SHA_D3D12=098f4f07182773b7420fe7e2558ec537fdac300489cfde5f52e1df5678ac5ef3
SHA_D3D12CORE=741a46b80cf538ef5133b2c35f0f2833677815ef37a6188a6b5c1db02d03fbc4
ROOT=$(cd "$(dirname "$0")/.." && pwd)
COMMON=$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)
GAMEDIR=${1:-$(dirname "$COMMON")/tagpu/gamedir}
[ -d "$GAMEDIR" ] || { echo "no gamedir at $GAMEDIR" >&2; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

URL="https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/$VER/microsoft.ml.onnxruntime.directml.$VER.nupkg"
echo "fetching $URL"
curl -sSL --max-time 600 -o "$TMP/ort.nupkg" "$URL"
unzip -o -q "$TMP/ort.nupkg" runtimes/win-x86/native/onnxruntime.dll LICENSE ThirdPartyNotices.txt -d "$TMP/ort"
echo "$SHA_ORT  $TMP/ort/runtimes/win-x86/native/onnxruntime.dll" | sha256sum -c -
cp "$TMP/ort/runtimes/win-x86/native/onnxruntime.dll" "$GAMEDIR/onnxruntime.dll"
cp "$TMP/ort/LICENSE" "$GAMEDIR/onnxruntime_LICENSE.txt"
cp "$TMP/ort/ThirdPartyNotices.txt" "$GAMEDIR/onnxruntime_ThirdPartyNotices.txt"

URL="https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/$DML_VER/microsoft.ai.directml.$DML_VER.nupkg"
echo "fetching $URL"
curl -sSL --max-time 600 -o "$TMP/dml.nupkg" "$URL"
unzip -o -q "$TMP/dml.nupkg" bin/x86-win/DirectML.dll LICENSE.txt ThirdPartyNotices.txt -d "$TMP/dml"
echo "$SHA_DML  $TMP/dml/bin/x86-win/DirectML.dll" | sha256sum -c -
cp "$TMP/dml/bin/x86-win/DirectML.dll" "$GAMEDIR/DirectML.dll"
cp "$TMP/dml/LICENSE.txt" "$GAMEDIR/DirectML_LICENSE.txt"
cp "$TMP/dml/ThirdPartyNotices.txt" "$GAMEDIR/DirectML_ThirdPartyNotices.txt"

URL="https://github.com/HansKristian-Work/vkd3d-proton/releases/download/v$VKD3D_VER/vkd3d-proton-$VKD3D_VER.tar.zst"
echo "fetching $URL"
curl -sSL --max-time 600 -o "$TMP/vkd3d.tar.zst" "$URL"
tar --zstd -xf "$TMP/vkd3d.tar.zst" -C "$TMP" "vkd3d-proton-$VKD3D_VER/x86"
echo "$SHA_D3D12  $TMP/vkd3d-proton-$VKD3D_VER/x86/d3d12.dll" | sha256sum -c -
echo "$SHA_D3D12CORE  $TMP/vkd3d-proton-$VKD3D_VER/x86/d3d12core.dll" | sha256sum -c -
cp "$TMP/vkd3d-proton-$VKD3D_VER/x86/d3d12.dll" "$GAMEDIR/d3d12.dll"
cp "$TMP/vkd3d-proton-$VKD3D_VER/x86/d3d12core.dll" "$GAMEDIR/d3d12core.dll"

cp "$ROOT/unditherer/models/full.onnx" "$GAMEDIR/full.onnx"
echo "installed onnxruntime $VER (win-x86, DirectML), DirectML $DML_VER,"
echo "vkd3d-proton $VKD3D_VER (x86) and full.onnx into $GAMEDIR"
