#!/bin/bash
# thread-split-check.sh — the build rule of the frame packet exchange
# (research/notes/frame-packet-exchange.html §10), in census mode since landing 1.
#
#   tools/thread-split-check.sh [tagpu/ddraw]        # the check: exit 0 clean, 1 offender(s)
#   tools/thread-split-check.sh --census [tagpu/ddraw]  # print every file that hits, and why
#
# RULE. Only a file on tagpu/ddraw/thread-split.allow may (a) name an engine virtual
# address, (b) include inc/tagpu_engine.h, (c) probe with IsBad*Ptr, or (d) add an
# offset to the engine's main pointer. Everything else under src/ and inc/ is checked
# with its comments stripped, and one hit fails the build. The list is DEFAULT-DENY
# and started FULL (every file that named engine memory on 2026-09-12, each with its
# class and its argument); it only shrinks, one line per landing that converts a file,
# so a conversion's diff shows what left the render thread. Adding a line is the
# owner's decision, made in a review, never a session's fix for a red build.
#
# PATTERNS (perl, case-insensitive, after `/* */` and `//` comments are removed):
#   VA       \b0x0*(4[0-9a-f]{5}|5[0-2][0-9a-f]{4})(?![0-9a-f])
#            0x4xxxxx (.text, from 0x401000) and 0x5[0-2]xxxx (.rdata from 0x4FC000, .data
#            from 0x501000 and its bss up to the .tls at 0x52C000: the main pointer
#            0x511DE8, the graphics globals 0x51FBD0, the debris slots 0x511DF0.., the
#            order descriptors 0x512344, the allocator's flag byte 0x5289A4). The
#            suffix-aware tail is what catches the `0x00511DE8u` spelling 19 files use;
#            `grep 0x511DE8` matched 6 of 25.
#   INCLUDE  #\s*include\s*"tagpu_engine\.h"
#   PROBE    \bIsBad(Read|Write|Code|String)Ptr\w*
#   CONDUIT  \b(ta|main|main_p)\s*\+   an offset added to the main pointer, whatever
#            the offset is spelled as (a literal, or a macro from a header)
#
# What it does NOT prove: that a file on the list reads only what its class says.
# That is the review's job; the list's argument column is what the review reads.
#
# Runs as a prerequisite of ddraw.dll in tagpu/ddraw/Makefile, so `make -C tagpu/ddraw`
# on a desk and in CI (.github/workflows/build.yml) both fail on an offender. The
# upstream build.cmd / vcxproj do not run it. Needs perl (git's own dependency).
set -u
MODE=check
if [ "${1:-}" = "--census" ]; then MODE=census; shift; fi
DIR="${1:-$(cd "$(dirname "$0")/../tagpu/ddraw" && pwd)}"
ALLOW="$DIR/thread-split.allow"
cd "$DIR" || { echo "thread-split: no such directory: $DIR" >&2; exit 2; }
command -v perl >/dev/null 2>&1 || { echo "thread-split: perl is required" >&2; exit 2; }

# third-party headers carried by the fork: not ours, never scanned
EXCLUDE='^inc/(glcorearb|wglext|d3d9shader|d3dcaps|ddraw|KHR/.*)\.h$'

scan() {   # $1 = file; prints "line: TAG: text" for every hit, comments stripped
    perl -0777 -ne '
        s{/\*.*?\*/}{ $& =~ s/[^\n]//gr }gse;    # block comments: keep the newlines, drop the text
        s{//[^\n]*}{}g;                           # line comments
        my $n = 0;
        for my $l (split /\n/, $_) {
            $n++;
            my @tags;
            push @tags, "VA"      if $l =~ /\b0x0*(4[0-9a-f]{5}|5[0-2][0-9a-f]{4})(?![0-9a-f])/i;
            push @tags, "INCLUDE" if $l =~ /#\s*include\s*"tagpu_engine\.h"/;
            push @tags, "PROBE"   if $l =~ /\bIsBad(Read|Write|Code|String)Ptr\w*/;
            push @tags, "CONDUIT" if $l =~ /\b(ta|main|main_p)\s*\+/;
            next unless @tags;
            $l =~ s/^\s+//; $l = substr($l, 0, 96);
            print "$n: ", join("+", @tags), ": $l\n";
        }' "$1"
}

# the allow-list: path, class, then the argument after `#`
declare -A ALLOWED
if [ -f "$ALLOW" ]; then
    while IFS= read -r line; do
        line="${line%%#*}"; line="$(echo "$line" | xargs 2>/dev/null)"
        [ -z "$line" ] && continue
        path="${line%% *}"; class="${line#* }"
        case "$class" in
            publisher|session-reader|fenced|tooling|pure-engine-code|engine-map|to-convert:*) ;;
            *) echo "thread-split: $ALLOW: unknown class '$class' for $path" >&2; exit 2 ;;
        esac
        [ -e "$path" ] || { echo "thread-split: $ALLOW names a file that does not exist: $path" >&2; exit 2; }
        ALLOWED["$path"]="$class"
    done < "$ALLOW"
elif [ "$MODE" = check ]; then
    echo "thread-split: missing $ALLOW" >&2; exit 2
fi

status=0
listed_clean=()
for f in src/*.c src/*.h inc/*.h; do
    [ -e "$f" ] || continue
    echo "$f" | grep -Eq "$EXCLUDE" && continue
    hits="$(scan "$f")"
    if [ "$MODE" = census ]; then
        [ -n "$hits" ] && { echo "== $f ${ALLOWED[$f]:+[${ALLOWED[$f]}]}"; echo "$hits" | head -6 | sed 's/^/   /'; }
        continue
    fi
    if [ -n "${ALLOWED[$f]:-}" ]; then
        [ -z "$hits" ] && listed_clean+=("$f")
        continue
    fi
    if [ -n "$hits" ]; then
        echo "thread-split: $f is NOT on $ALLOW and names engine memory:" >&2
        echo "$hits" | head -8 | sed 's/^/    /' >&2
        status=1
    fi
done
if [ "$MODE" = check ]; then
    if [ $status -ne 0 ]; then
        echo "thread-split: FAILED — a render-thread file may not read engine memory (frame-packet-exchange §10)." >&2
        echo "thread-split: a game-thread publisher belongs on the list WITH its argument; that is a review decision." >&2
    else
        n=${#ALLOWED[@]}
        echo "thread-split: clean — ${n} listed file(s), every other source names no engine memory"
        for f in "${listed_clean[@]}"; do echo "thread-split: note: $f is listed but names nothing any more — its line can go"; done
    fi
fi
exit $status
