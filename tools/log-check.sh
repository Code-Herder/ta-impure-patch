#!/bin/bash
# log-check.sh — only the log sink opens a log file (tagpu/ddraw/inc/tagpu_log.h).
#
#   tools/log-check.sh [tagpu/ddraw]     # exit 0 clean, 1 offender(s), 2 could not run
#
# RULE. No source under src/ or inc/ other than src/tagpu_log.c may pass a string literal
# ending in `.log` to fopen, _wfopen, _open, OpenFile or CreateFile*. Every line goes
# through tagpu_log(), whose caps hold only because it is the one writer: a module that
# opens tagpu.log itself writes past every cap and splits a rotation's bookkeeping.
#
# Comments are stripped first, so a comment that quotes the old pattern is not a hit. A
# path assembled at run time or held in a variable is not caught — a ratchet against the
# spelling every writer used, not a proof; the review covers the rest.
#
# Runs as a prerequisite of ddraw.dll in tagpu/ddraw/Makefile, beside thread-split and
# spirv, so a desk build and CI both fail on an offender. Needs perl.
set -u
DIR="${1:-$(dirname "$0")/../tagpu/ddraw}"
DIR="$(cd "$DIR" 2>/dev/null && pwd)" || { echo "log-check: no such directory: ${1:-tagpu/ddraw}" >&2; exit 2; }
cd "$DIR" || exit 2
command -v perl >/dev/null 2>&1 || { echo "log-check: perl is required" >&2; exit 2; }

hits=$(for f in src/*.c src/*/*.c inc/*.h; do
    [ -f "$f" ] || continue
    [ "$f" = src/tagpu_log.c ] && continue
    perl -0777 -ne '
        s{/\*.*?\*/}{ $& =~ s/[^\n]//gr }gse;
        s{//[^\n]*}{}g;
        my $n = 0;
        for my $l (split /\n/, $_, -1) {
            $n++;
            print "$ARGV:$n: $l\n"
                if $l =~ /\b(?:_?w?fopen|_open|OpenFile|CreateFile[AW]?)\s*\(\s*"[^"]*\.log"/;
        }' "$f"
done)

if [ -n "$hits" ]; then
    echo "log-check: a log file is opened outside tagpu_log.c — write through tagpu_log() instead:" >&2
    echo "$hits" >&2
    exit 1
fi
exit 0
