#!/usr/bin/env bash
# Fetch the pinned glslang that tools/spirv-gen.py compiles the fork's shaders
# with (Phase G / G19c). tools/glslang-vendor.json is the manifest; the unpacked
# tree lands in tools/glslang/, which is gitignored.
#
# NOTHING IN ANY BUILD CALLS THIS. The SPIR-V headers are generated here and
# committed as text; `make` only checks that they still match the GLSL they were
# made from (tools/spirv-check.sh), and that check needs the C preprocessor and
# python3 and nothing else. Run this only when a shader has changed and the
# headers have to be regenerated.
#
# The archive is pinned by version AND by hash, so a release re-cut under the
# same tag does not silently change what the shaders were compiled by.
set -euo pipefail

repo="$(git rev-parse --show-toplevel)"
manifest="$repo/tools/glslang-vendor.json"
dest="$repo/tools/glslang"

read -r ver url sha name <<<"$(python3 - "$manifest" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
name, f = next(iter(m["files"].items()))
print(m["version"], f["url"], f["sha256"], name)
PY
)"

if [ -x "$dest/bin/glslang" ] && "$dest/bin/glslang" --version 2>/dev/null | grep -q "$ver"; then
    echo "glslang $ver is already in tools/glslang/"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
echo "fetching glslang $ver"
curl -fsSL -o "$tmp/$name" "$url"

got="$(sha256sum "$tmp/$name" | cut -d' ' -f1)"
if [ "$got" != "$sha" ]; then
    echo "glslang-fetch: the archive does not match the manifest" >&2
    echo "  expected $sha" >&2
    echo "  got      $got" >&2
    exit 1
fi

rm -rf "$dest"
mkdir -p "$dest"
tar xzf "$tmp/$name" -C "$dest"
"$dest/bin/glslang" --version | head -1
echo "unpacked into tools/glslang/ -- now: tools/spirv-gen.py"
