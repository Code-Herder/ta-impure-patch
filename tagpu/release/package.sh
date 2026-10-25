#!/usr/bin/env bash
# Assemble the release folder and its zip under dist/: the built ddraw.dll, which
# carries the Classic++ restorer's models, and README.txt. CI runs it after
# `make -C tagpu/ddraw`; locally the same two commands give the same zip.
# VERSION overrides the `git describe` name (CI passes the tag).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
version="${VERSION:-$(git -C "$root" describe --tags --always --dirty 2>/dev/null || echo unknown)}"
name="ta-impure-patch-${version}"
out="$root/dist/$name"
dll="$root/tagpu/ddraw/ddraw.dll"

[ -f "$dll" ] || { echo "package: $dll is not built -- make -C tagpu/ddraw first" >&2; exit 1; }

rm -rf "$out"
mkdir -p "$out"
cp "$dll" "$out/ddraw.dll"
sed "s/@VERSION@/$version/g" "$here/README.txt" > "$out/README.txt"

( cd "$root/dist" && rm -f ta-impure-patch-*.zip && zip -q -r "$name.zip" "$name" )
ls -l "$out"
echo "$root/dist/$name.zip"
