#!/bin/bash
# sim-state-check.sh — the simulation-state rule (tagpu/ddraw/inc/tagpu_simreg.h;
# research/notes/tadr-port/demo-recorder-completeness.md).
#
#   tools/sim-state-check.sh <tagpu/ddraw> <object suffix>    # .o, or .stock.o for LIMITS=stock
#   exit 0 clean, 1 offender(s), 2 could not run
#
# For every source file marked `sim` in thread-split.allow, on the object about to be linked:
#   UNDECLARED  a writable static (nm b B d D C) with no `simreg_` twin of the same name, counted:
#               two function-local statics `top` need two declarations of `top` (GCC's `.N`
#               suffixes are stripped). A name match, because GCC relocates file statics against
#               the section symbol, so nothing pairs them by address.
#   STALE       a declaration whose variable is not a writable static of that object.
#   IMPORT      a call to a function on the REFUSED list below: the C and Win32 allocators, TLS and
#               fiber-local slots, page protection, file mappings, another process's memory, and
#               GetProcAddress, which would reach any of them around the list. Memory, TLS
#               indices and page changes come through tagpu_simreg.h and tagpu_detour.h.
#   LEVER       (the source, comments and #include lines removed) a string literal that is a whole
#               `tagpu_` file name anywhere but directly inside tagpu_sim_lever(,
#               tagpu_sim_lever_read( or SIM_TOOL_FILE(.
#
# Not proven, by construction of these rules, and so the review's to check and later the replay
# harness's:
#   - that a declaration's class is right, and that a SIM_TOOL_FILE is only tooling's -- each is
#     a claim the code makes, like a class;
#   - state kept by a file that is not marked `sim`, or in memory a presentation module or
#     tagpu_cob_safe.c allocates;
#   - a lever named through a macro, a string built at run time or a header;
#   - an allocator or page change the REFUSED list does not name, or one reached through the
#     engine: its own helpers or import slots by address (the def array's open and seal,
#     0x4D8780 and 0x4D8710, are the one use, and the engine's own protocol for that array);
#   - a variable declared twice, which can hide an undeclared namesake from the count by name;
#     the registry's walk at attach refuses two declarations of one byte instead.
#
# Runs inside the link recipe of tagpu/ddraw/Makefile, so it reads the objects the DLL is linked
# from, after all of them are built, whenever either build links (the landing gate builds both).
set -u
DIR="${1:?usage: sim-state-check.sh <tagpu/ddraw> <object suffix>}"
EXT="${2:?usage: sim-state-check.sh <tagpu/ddraw> <object suffix>}"
DIR="$(cd "$DIR" 2>/dev/null && pwd)" || { echo "sim-state: no such directory: $1" >&2; exit 2; }
ALLOW="$DIR/thread-split.allow"
NM="${NM:-i686-w64-mingw32-nm}"
command -v "$NM" >/dev/null 2>&1 || { echo "sim-state: $NM is required" >&2; exit 2; }
command -v perl >/dev/null 2>&1 || { echo "sim-state: perl is required" >&2; exit 2; }
[ -f "$ALLOW" ] || { echo "sim-state: missing $ALLOW" >&2; exit 2; }

# The C runtime's allocators, the Win32 heap, global, local and virtual memory, the native API
# under them, TLS and fiber-local slots, page protection, file mappings, another process's memory.
REFUSED='malloc|calloc|realloc|free|_recalloc|_expand|_aligned_malloc|_aligned_realloc|_aligned_recalloc|_aligned_offset_malloc|_aligned_free|_malloca|_freea|_strdup|strdup|_strndup|strndup|_wcsdup|wcsdup|asprintf|vasprintf|__mingw_asprintf|__mingw_vasprintf|'
REFUSED+='HeapAlloc|HeapReAlloc|HeapFree|HeapCreate|HeapDestroy|GlobalAlloc|GlobalReAlloc|GlobalFree|LocalAlloc|LocalReAlloc|LocalFree|CoTaskMemAlloc|CoTaskMemRealloc|CoTaskMemFree|'
REFUSED+='VirtualAlloc|VirtualAllocEx|VirtualAllocExNuma|VirtualAlloc2|VirtualFree|VirtualFreeEx|VirtualProtect|VirtualProtectEx|'
REFUSED+='RtlAllocateHeap|RtlReAllocateHeap|RtlFreeHeap|NtAllocateVirtualMemory|NtFreeVirtualMemory|NtProtectVirtualMemory|ZwAllocateVirtualMemory|ZwFreeVirtualMemory|ZwProtectVirtualMemory|'
REFUSED+='CreateFileMappingA|CreateFileMappingW|MapViewOfFile|MapViewOfFileEx|UnmapViewOfFile|WriteProcessMemory|'
REFUSED+='TlsAlloc|TlsFree|FlsAlloc|FlsFree|GetProcAddress'

files=()
while IFS= read -r line; do
    line="${line%%#*}"
    read -r path class scope rest <<< "$line"
    [ -z "${path:-}" ] && continue
    [ "${scope:-}" = sim ] || continue
    case "$path" in
        *.c) files+=("$path") ;;
        *) echo "sim-state: $ALLOW marks $path sim: only a source file can be, since a header's statics belong to the objects that include it" >&2; exit 2 ;;
    esac
done < "$ALLOW"
[ ${#files[@]} -gt 0 ] || { echo "sim-state: $ALLOW marks no file sim" >&2; exit 2; }

status=0
decls=0
for src in "${files[@]}"; do
    obj="$DIR/${src%.c}$EXT"
    [ -f "$obj" ] || { echo "sim-state: $obj is missing (build it first)" >&2; exit 2; }
    syms="$("$NM" "$obj")" || { echo "sim-state: $NM failed on $obj" >&2; exit 2; }
    # writable statics and their declarations, as "name count" lines
    wr="$(echo "$syms" | awk 'NF == 3 && $2 ~ /^[bBdDC]$/ && $3 !~ /^\./ { n = $3; sub(/^_/, "", n); sub(/\.[0-9]+$/, "", n); print n }' | sort | uniq -c | awk '{ print $2, $1 }')"
    dc="$(echo "$syms" | awk 'NF == 3 && $2 ~ /^[rR]$/ && $3 ~ /^_?simreg_/ { n = $3; sub(/^_?simreg_/, "", n); sub(/\.[0-9]+$/, "", n); print n }' | sort | uniq -c | awk '{ print $2, $1 }')"
    decls=$((decls + $(echo "$dc" | awk 'NF { s += $2 } END { print s + 0 }')))
    out="$(join -a1 -a2 -e0 -o 0,1.2,2.2 <(echo "$wr") <(echo "$dc") | awk -v src="$src" '
        $2 > $3 && $3 == 0 { printf "sim-state: UNDECLARED %s: %s (%d definition(s), no declaration)\n", src, $1, $2; next }
        $2 > $3            { printf "sim-state: UNDECLARED %s: %s (%d definition(s), %d declaration(s))\n", src, $1, $2, $3; next }
        $3 > $2 && $2 == 0 { printf "sim-state: STALE %s: %s is declared but is no writable static of this object\n", src, $1; next }
        $3 > $2            { printf "sim-state: STALE %s: %s (%d declaration(s), %d definition(s))\n", src, $1, $3, $2; next }')"
    imp="$(echo "$syms" | awk '$1 == "U" { n = $2; sub(/^__imp_/, "", n); sub(/^_/, "", n); sub(/@[0-9]+$/, "", n); print n }' | grep -Ex "$REFUSED" | sort -u | sed "s|^|sim-state: IMPORT $src calls |")"
    lev="$(perl -0777 -ne '
        s{/\*.*?\*/}{ $& =~ s/[^\n]//gr }gse;       # block comments, newlines kept
        s{//[^\n]*}{}g;                              # line comments
        s{^[ \t]*#[ \t]*include[^\n]*}{}mg;          # #include "tagpu_x.h" names a header, not a file
        my $n = 0;
        for my $l (split /\n/, $_) {
            $n++;
            while ($l =~ /"(tagpu_[A-Za-z0-9_]+\.[A-Za-z]+)"/g) {
                my ($name, $at) = ($1, $-[0]);
                next if substr($l, 0, $at) =~ /\b(tagpu_sim_lever|tagpu_sim_lever_read|SIM_TOOL_FILE)\s*\(\s*$/;
                print "$n: $name\n";
            }
        }' "$DIR/$src" | sed "s|^|sim-state: LEVER $src:|")"
    for r in "$out" "$imp" "$lev"; do
        [ -n "$r" ] && { echo "$r" >&2; status=1; }
    done
done
if [ $status -ne 0 ]; then
    echo "sim-state: FAILED — every writable static of a sim file is declared with tagpu_simreg.h, and memory, TLS, page changes and levers come through its wrappers (inc/tagpu_simreg.h)." >&2
else
    echo "sim-state: clean — ${#files[@]} sim file(s), $decls declaration(s) ($EXT)"
fi
exit $status
