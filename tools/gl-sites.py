#!/usr/bin/env python3
"""Count the GL call sites left in the fork -- the vulkan-only plan's exit condition.

Gate 11-5's exit condition is a NUMBER ("no GL call sites remain in
tagpu/ddraw/src/*.c").  Produced by a different throwaway script each time, that
is not an exit condition, it is an assertion: two sessions counting "GL sites"
can differ by a hundred and both be telling the truth, because the disagreement
is in the three things nobody wrote down.  All three are decisions, not facts,
so they are made here, once:

  THE PATTERN.  `\\b(?:gl|x_gl)[A-Z][A-Za-z0-9]*\\s*\\(` -- the GL naming
      convention (`gl` + CapitalisedVerb) plus this tree's `x_gl` prefix for an
      entry point fetched through `wglGetProcAddress`.  The capital is what keeps
      `glog(` and `global_thing(` out.  `--wide` adds `oglu_`, `wgl` and `xwgl`:
      the fork's own GL wrappers and the context/extension layer, which are GL
      dependencies but not GL calls.  Both numbers are reported, because the
      narrow one is the exit condition and the wide one is what says whether a
      file is really free of the API or merely of its draw calls.

  THE TEXT.  Comments and string literals are MASKED before matching.  This is
      the difference that bites: these files document what they deleted, so a
      tombstone naming `glTexParameterf` counts as a call site under a plain
      grep and the number goes UP as the work goes forward.  Newlines survive the
      masking so that `--list` reports true line numbers.

  THE FILE SET.  `tagpu/ddraw/src/*.c` only.  Not headers -- `opengl_utils.h` and
      the fork's GL headers DECLARE the whole API and always will, so including
      them measures the declarations rather than the calls.  Not `tagpu/src/**`,
      which is the other DLL.

Run it from anywhere in the repo:

    tools/gl-sites.py                 # the table and the two totals
    tools/gl-sites.py --wide          # sort by the wide count instead
    tools/gl-sites.py --list FILE     # every site in one file, with line numbers
    tools/gl-sites.py --json          # the same numbers for a script to diff

Exit status is 0 when the narrow total is 0 -- i.e. when gate 11-5's exit
condition is met -- and 1 otherwise, so a landing can gate on it directly.
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

NARROW = re.compile(r"\b(?:gl|x_gl)[A-Z][A-Za-z0-9]*\s*\(")
# WIDE is NARROW plus the three families that are GL without being spelled
# `glSomething`: `oglu_*` (the fork's own GL helpers), `wgl*` and `xwgl*`.
#
# IT KEEPS NARROW'S `[A-Z]` FOR THE BARE `gl` PREFIX. `[A-Za-z_]` would make
# `gl` followed by a lower-case letter a match and sweep in three of this fork's
# own identifiers: `glog`, the per-module logger that writes to tagpu.log,
# `glyph_obj` / `glyph_raster` / `glyph_block_*`, and `gl_probe` -- enough to
# report files such as `tagpu_gui_hook.c` and `tagpu_text.c` as carrying GL
# while they carry none at all.
WIDE = re.compile(
    r"\b(?:(?:gl|x_gl)[A-Z][A-Za-z0-9]*|(?:oglu_|wgl|xwgl)[A-Za-z_][A-Za-z0-9_]*)\s*\("
)


def mask(t):
    """Blank out comments and string literals, keeping every newline.

    Line numbers have to survive so that --list points at a real line, and the
    blanking has to happen at all so that a comment naming a deleted GL call is
    not counted as one.
    """
    out = list(t)
    i, n = 0, len(t)
    while i < n:
        c = t[i]
        if c == "/" and i + 1 < n and t[i + 1] == "*":
            j = t.find("*/", i)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if t[k] != "\n":
                    out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and t[i + 1] == "/":
            j = t.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c in "\"'":
            q = c
            j = i + 1
            while j < n and t[j] != q:
                j += 2 if t[j] == "\\" else 1
            for k in range(i, min(j + 1, n)):
                if t[k] != "\n":
                    out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out)


def repo_root():
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            capture_output=True, text=True, check=True,
        )
        return Path(out.stdout.strip())
    except Exception:
        return Path(__file__).resolve().parents[1]


def sources(root):
    return sorted((root / "tagpu" / "ddraw" / "src").glob("*.c"))


def main():
    ap = argparse.ArgumentParser(add_help=True, description=__doc__.splitlines()[0])
    ap.add_argument("--wide", action="store_true",
                    help="sort the table by the wide count")
    ap.add_argument("--list", metavar="FILE",
                    help="print every site in one file, with line numbers")
    ap.add_argument("--json", action="store_true",
                    help="emit the numbers as JSON")
    args = ap.parse_args()

    root = repo_root()
    files = sources(root)
    if not files:
        print("gl-sites: no sources under tagpu/ddraw/src -- wrong repo?",
              file=sys.stderr)
        return 2

    if args.list:
        want = Path(args.list).name
        hit = [p for p in files if p.name == want]
        if not hit:
            print("gl-sites: %s is not in tagpu/ddraw/src" % want, file=sys.stderr)
            return 2
        masked = mask(hit[0].read_text(errors="replace"))
        for ln, line in enumerate(masked.splitlines(), 1):
            for m in NARROW.finditer(line):
                print("%s:%d: %s" % (want, ln, m.group(0).rstrip("(").strip()))
        return 0

    rows, tot_n, tot_w = [], 0, 0
    for p in files:
        masked = mask(p.read_text(errors="replace"))
        n = len(NARROW.findall(masked))
        w = len(WIDE.findall(masked))
        tot_n += n
        tot_w += w
        if n or w:
            rows.append({"file": p.name, "narrow": n, "wide": w})

    bearing = [r for r in rows if r["narrow"]]

    if args.json:
        print(json.dumps({
            "narrow_total": tot_n,
            "wide_total": tot_w,
            "gl_bearing_files": len(bearing),
            "files": rows,
        }, indent=2))
    else:
        key = "wide" if args.wide else "narrow"
        print("%5s  %5s  %s" % ("calls", "wide", "file"))
        for r in sorted(rows, key=lambda r: (-r[key], r["file"])):
            if not r["narrow"] and not args.wide:
                continue
            print("%5d  %5d  %s" % (r["narrow"], r["wide"], r["file"]))
        print("%5d  %5d  TOTAL  (%d file(s) with a GL call)"
              % (tot_n, tot_w, len(bearing)))

    return 0 if tot_n == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
