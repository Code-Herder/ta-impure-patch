#!/usr/bin/env bash
# amalgamate.sh -- regenerate the vendored zstd from its release tarball.
#
#   tools/zstd/amalgamate.sh zstd-1.5.7.tar.gz
#
# Writes tagpu/ddraw/src/zstd.c (upstream's single-file amalgamation of lib/, configured by
# tools/zstd/zstd-in.c), tagpu/ddraw/inc/zstd.h and inc/zstd_errors.h (the public API, as
# released) and inc/zstd_LICENSE.txt (the release's BSD LICENSE, verbatim).
#
# NOTHING IN ANY BUILD CALLS THIS. The four files are committed; run this only to change the
# configuration in zstd-in.c or to move to another release, which also changes the pinned hash
# below.
#
# The tarball is the release asset https://github.com/facebook/zstd/releases/download/v1.5.7/
# zstd-1.5.7.tar.gz, pinned by its SHA-256 (the one the release publishes beside it), so a re-cut
# under the same tag cannot change what is vendored. Its contents are data: it is unpacked into a
# fresh directory and upstream's combine.py runs from outside it, with python3 -I, so nothing
# beside the script or in the working directory is imported.
set -euo pipefail

VERSION=1.5.7
SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3

[ $# -eq 1 ] || { echo "usage: $0 zstd-$VERSION.tar.gz" >&2; exit 2; }
tarball="$(realpath "$1")"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ddraw="$(realpath "$here/../../tagpu/ddraw")"

have="$(sha256sum "$tarball" | cut -d' ' -f1)"
if [ "$have" != "$SHA256" ]; then
    echo "amalgamate: $tarball has SHA-256 $have, not zstd $VERSION's $SHA256" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir "$work/src" "$work/run"
tar -xzf "$tarball" -C "$work/src" --no-same-owner --no-same-permissions
src="$work/src/zstd-$VERSION"
[ -d "$src/lib" ] || { echo "amalgamate: $tarball holds no zstd-$VERSION/lib" >&2; exit 1; }

# -x legacy/zstd_legacy.h: ZSTD_LEGACY_SUPPORT is 0, so no legacy decoder is inlined.
(cd "$work/run" && python3 -I "$src/build/single_file_libs/combine.py" \
    -r "$src/lib" -x legacy/zstd_legacy.h -o "$work/zstd.c" "$here/zstd-in.c") >/dev/null 2>"$work/combine.log" || {
    cat "$work/combine.log" >&2
    echo "amalgamate: combine.py failed" >&2
    exit 1
}
if grep -q '^Error:' "$work/combine.log"; then
    grep '^Error:' "$work/combine.log" >&2
    exit 1
fi

install -m 644 "$work/zstd.c"            "$ddraw/src/zstd.c"
install -m 644 "$src/lib/zstd.h"         "$ddraw/inc/zstd.h"
install -m 644 "$src/lib/zstd_errors.h"  "$ddraw/inc/zstd_errors.h"
install -m 644 "$src/LICENSE"            "$ddraw/inc/zstd_LICENSE.txt"
echo "amalgamate: zstd $VERSION -> src/zstd.c, inc/zstd.h, inc/zstd_errors.h, inc/zstd_LICENSE.txt"
