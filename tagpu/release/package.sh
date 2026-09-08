#!/usr/bin/env bash
# Assemble the release folder and its zip under dist/: the built ddraw.dll, the ini it
# wants, the Classic++ restorer's weights and README.txt. CI runs it after
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
for w in full tiny; do
    [ -f "$root/unditherer/models/$w.w32.bin" ] || { echo "package: unditherer/models/$w.w32.bin missing" >&2; exit 1; }
done

rm -rf "$out"
mkdir -p "$out"
cp "$dll" "$out/ddraw.dll"
cp "$here/ddraw.ini" "$out/ddraw.ini"
sed "s/@VERSION@/$version/g" "$here/README.txt" > "$out/README.txt"
cp "$root/unditherer/models/full.w32.bin" "$root/unditherer/models/tiny.w32.bin" "$out/"

( cd "$root/dist" && rm -f "$name.zip" && zip -q -r "$name.zip" "$name" )
ls -l "$out"
echo "$root/dist/$name.zip"
