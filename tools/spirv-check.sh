#!/bin/bash
# spirv-check.sh -- the build rule of the shader pipeline (roadmap.md Phase G).
#
#   tools/spirv-check.sh [tagpu/ddraw]     # exit 0 current, 1 stale, 2 could not run
#
# WHAT IT ASSERTS. The SPIR-V under tagpu/ddraw/inc/spirv/ is generated from the GLSL
# string literals in tagpu/ddraw/src/*.c by tools/spirv-gen.py, and committed as text.
# Generated code that is committed rots, so two things are checked here, on every
# build, in the same place and for the same reason as thread-split-check.sh:
#
#   1. FRESHNESS. Every header carries the SHA-256 of the Vulkan GLSL each shader was
#      compiled from, and the hash of spirv-gen.py's own transform. This re-runs the
#      extraction and the transform -- the C preprocessor and python3, never glslang --
#      and compares. A shader edited in its C string, or a transform edited in the
#      tool, fails the build and names what moved.
#   2. THE HEADERS COMPILE AS C. Only the shaders a ported pass actually uses are
#      #included anywhere, so most of these arrays are never seen by the compiler and
#      a malformed one would sit there until a pass first included it. Each header
#      is syntax-checked standalone.
#
# WHAT IT DOES NOT ASSERT: that the SPIR-V is what glslang would emit TODAY. That
# would need glslang, which is deliberately not a build dependency (tools/spirv-gen.py's
# header says why), and the version is pinned in tools/glslang-vendor.json anyway.
# The hash chain covers the input and the transform; the compiler is pinned.
#
# NEEDS python3 and the cross compiler, both of which every build that runs the
# Makefile already has. The upstream build.cmd / vcxproj do not run it, exactly as
# they do not run thread-split-check.sh.
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ddraw="${1:-$here/../tagpu/ddraw}"
cc="${CC:-i686-w64-mingw32-gcc}"

command -v python3 >/dev/null 2>&1 || { echo "spirv-check: no python3" >&2; exit 2; }

# THE EXIT CODE IS PASSED THROUGH, NOT FLATTENED. spirv-gen exits 1 for "stale"
# and 2 for "could not run at all" (no preprocessor, a shader it cannot read);
# `|| exit 1` would report the second as the first, and send someone to
# regenerate headers that are perfectly current.
CC="$cc" python3 "$here/spirv-gen.py" --check || exit $?

shopt -s nullglob
hdrs=("$ddraw"/inc/spirv/*.spv.h)
if [ ${#hdrs[@]} -eq 0 ]; then
    echo "spirv-check: tagpu/ddraw/inc/spirv/ is empty -- run tools/spirv-gen.py" >&2
    exit 1
fi
for h in "${hdrs[@]}"; do
    # -Wno-unused: these arrays are static and this translation unit uses none of
    # them, which is the whole point of checking them here.
    if ! "$cc" -fsyntax-only -Wno-unused -x c "$h" 2>&1; then
        echo "spirv-check: $h does not compile" >&2
        exit 1
    fi
done
exit 0
