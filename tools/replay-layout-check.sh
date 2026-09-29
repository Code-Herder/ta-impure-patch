#!/bin/bash
# replay-layout-check.sh -- the build rule of the replay layout (tools/replay-layout-gen.py).
#
#   tools/replay-layout-check.sh [tagpu/ddraw]     # exit 0 current, 1 stale or invalid, 2 could not run
#
# WHAT IT ASSERTS. tagpu/ddraw/replay-layout.tsv lists the bytes of the engine's unit and player
# records the demo recorder does not record as plain values, and the recorder compiles that list
# from inc/tagpu_replay_layout.h, which is generated from the file and committed. Two things are
# checked on every build, in the same place and for the same reason as spirv-check.sh:
#
#   1. THE FILE IS VALID AND THE HEADER IS ITS OWN. The generator re-reads the file, validates
#      every row, renders the header and compares it with the committed one, whose SHA-256s of
#      the file and of the generator say which side moved. An exclusion added to the file without
#      regenerating, or to the header without the file, fails the build.
#   2. THE HEADER COMPILES AS C WITH NO WARNING, since nothing that includes it may yet use every
#      row: a value that does not fit its field is an error, not a truncation.
#
# NEEDS python3 and the cross compiler, which every build that runs the Makefile already has.
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ddraw="${1:-$here/../tagpu/ddraw}"
cc="${CC:-i686-w64-mingw32-gcc}"

command -v python3 >/dev/null 2>&1 || { echo "replay-layout-check: no python3" >&2; exit 2; }

# The generator's exit code is passed through: 1 is stale or invalid, 2 could not run.
python3 "$here/replay-layout-gen.py" --check || exit $?

h="$ddraw/inc/tagpu_replay_layout.h"
# -Wno-unused: this translation unit uses none of the table, which is the point of checking it here.
if ! "$cc" -fsyntax-only -Wall -Werror -Wno-unused -x c "$h" 2>&1; then
    echo "replay-layout-check: $h does not compile" >&2
    exit 1
fi
exit 0
