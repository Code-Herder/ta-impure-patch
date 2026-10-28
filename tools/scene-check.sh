#!/bin/bash
# scene-check.sh -- the scene rule (tagpu/ddraw/src/tagpu_scene.h).
#
#   tools/scene-check.sh <wiki objects...> -- <every other object of the link>
#                                              # exit 0 clean, 1 a finding, 2 could not run
#
# WHAT IT ASSERTS. The world passes are compiled twice: once for the game's world and once, with
# -DTAGPU_SCENE_WIKI, for the in-game wiki's stage (src/<name>.wiki.o). The two copies share no
# static -- C's linkage guarantees that -- but a wiki copy could still CALL into a module only the
# game's world has (the cast-shadow map, the Classic++ restorer, the game's own hand-overs) and so
# draw from or write into the game's state. tagpu_scene.h redirects every such call it knows of to
# tagpu_wikiworld.c; this check is what makes "every" true. For each undefined symbol of each wiki
# object it asks, in order:
#
#   1. is it defined by a wiki object?            the wiki's own instances -- allowed
#   2. is it defined by no object of ours at all? the C library, the compiler's helpers and the
#                                                 Win32 imports -- allowed
#   3. is it on ALLOW below?                      a stateless or read-only helper -- allowed
#
# and fails on anything else, naming the object and the symbol. A finding is a call tagpu_scene.h
# must redirect to an inert answer or to the wiki's producer, never a line to add to ALLOW unless
# the helper really keeps no state of the game's scene.
#
# WHAT IT DOES NOT ASSERT. That a symbol tagpu_scene.h renames is renamed everywhere: the link
# does that, by refusing a symbol two objects define. Nor anything about data the wiki copies are
# HANDED: what the producer passes them is the producer's argument.
#
# NEEDS the cross toolchain's nm, which every build that runs the Makefile already has. Run as a
# prerequisite of the link (Makefile, "THE SCENE RULE"), after every object is built.
set -uo pipefail

nm="${NM:-i686-w64-mingw32-nm}"
command -v "$nm" >/dev/null 2>&1 || { echo "scene-check: no $nm" >&2; exit 2; }

# THE ALLOW LIST: C names (no leading underscore), shell globs. Each is a helper that keeps none
# of the game's scene state, so calling it from the wiki's copy shares nothing with the game's.
ALLOW=(
    'tagpu_wikiworld_*'          # the wiki's producer and the inert answers (tagpu_wikiworld.c)
    'tagpu_pb_scratch_*'         # the bake's scratch, shared by construction (tagpu_scene.h)
    'tagpu_vk_stage_*'           # the banded upload, over a TAGPU_VKSTAGE the caller owns
    'tagpu_gaf_atlas_*'          # the shelf atlas, over a TAGPU_GAFATLAS the caller owns
    'tagpu_gaf_frame_*'          # GAF frame readers
    'tagpu_gaf_seq_*'
    'tagpu_gaf_rects_*'          # the atlas allowance's arithmetic
    'tagpu_pal_engine'           # the palettes, read
    'tagpu_pal_engine_serial'
    'tagpu_pal_expand'
    'tagpu_pal_gamma'
    'tagpu_vk_armed'             # device queries
    'tagpu_vk_owns_present'
    'tagpu_vk_max_image_dim'
    'tagpu_vk_max_storage_range'
    'tagpu_line_*'               # the lines' grid arithmetic
    'tagpu_log'                  # the log sink, which serialises its own writers
    'tagpu_logf'
    'tagpu_settings_ss'          # the Supersampling setting, read
    'tagpu_user_top_bound'       # the process's address-space top, written once at attach
)

wiki=() rest=() side=wiki
for a in "$@"; do
    if [ "$a" = "--" ]; then side=rest; continue; fi
    if [ "$side" = wiki ]; then wiki+=("$a"); else rest+=("$a"); fi
done
[ ${#wiki[@]} -gt 0 ] || { echo "scene-check: no wiki objects named" >&2; exit 2; }
[ ${#rest[@]} -gt 0 ] || { echo "scene-check: no other objects named" >&2; exit 2; }

defined() {
    "$nm" -g --defined-only "$@" 2>/dev/null | awk 'NF == 3 { print $3 }' | sort -u
}
wikidef="$(defined "${wiki[@]}")" || exit 2
ourdef="$(defined "${rest[@]}")" || exit 2
[ -n "$wikidef" ] && [ -n "$ourdef" ] || { echo "scene-check: nm read no symbols" >&2; exit 2; }

allowed() {
    local c="$1" g
    for g in "${ALLOW[@]}"; do
        # shellcheck disable=SC2053  # the glob is the point
        [[ "$c" == $g ]] && return 0
    done
    return 1
}

fail=0 checked=0
for o in "${wiki[@]}"; do
    [ -f "$o" ] || { echo "scene-check: $o does not exist" >&2; exit 2; }
    while read -r sym; do
        checked=$((checked + 1))
        grep -qxF -- "$sym" <<<"$wikidef" && continue
        grep -qxF -- "$sym" <<<"$ourdef" || continue
        c="${sym#_}"
        allowed "$c" && continue
        echo "scene-check: $o calls $c, which a module of the game's world defines;" \
             "redirect it in src/tagpu_scene.h" >&2
        fail=1
    done < <("$nm" -u "$o" | awk '{ print $NF }')
done

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "scene-check: clean -- ${#wiki[@]} wiki object(s), $checked undefined symbol(s)"
exit 0
