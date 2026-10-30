#!/usr/bin/env python3
"""The engine map's landing check: every address the code names is mapped, once.

  MISSING  an engine address on a line the landing added to tagpu/ or tools/ that no page of the
           engine map names (research/notes/exe-reverse-engineering.md, research/notes/engine/).
  EXTEND   an entry the landing added for a function that already has one elsewhere. An entry is
           a section whose heading names the address, or a table row whose first cell is it. Fold
           the new facts into the existing entry instead (the wiki's engine/address-index page
           lists every entry).

    tools/exemap-check.py              changes since main  (the landing gate)
    tools/exemap-check.py --base REF   changes since REF

Exit 1 if an address is MISSING. EXTEND does not fail the run: two entries are sometimes right
(a function seen from two subsystems), and the landing says why when it keeps both.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
NOTES = REPO / "research/notes"
MAP = ["research/notes/exe-reverse-engineering.md"] + sorted(
    p.relative_to(REPO).as_posix() for p in (NOTES / "engine").glob("*.md"))
ADDR = re.compile(r"\b0x0*([45][0-9A-Fa-f]{5})\b")


def norm(hexdigits: str) -> str:
    return "0x" + hexdigits.upper()


def git(*args) -> str:
    return subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, text=True,
                          check=True).stdout


def added(base: str, paths) -> dict:
    """{path: {line number: text}} of the lines added since base."""
    out, cur, n = {}, None, 0
    for ln in git("diff", "-U0", f"{base}...HEAD", "--", *paths).splitlines():
        if ln.startswith("+++ "):
            cur = ln[6:] if ln.startswith("+++ b/") else None
            continue
        m = re.match(r"@@ -\S+ \+(\d+)", ln)
        if m:
            n = int(m.group(1))
            continue
        if cur and ln.startswith("+"):
            out.setdefault(cur, {})[n] = ln[1:]
            n += 1
    return out


def entries(path: str):
    """(address, line number, section heading) for each entry in one page."""
    fence, section = False, ""
    for i, line in enumerate((REPO / path).read_text().split("\n"), 1):
        if line.startswith("```"):
            fence = not fence
        if fence:
            continue
        h = re.match(r"^#{1,6} (.*)", line)
        if h:
            section = h.group(1)
            cell = h.group(1)
        elif line.startswith("|") and line.count("|") >= 2:
            cell = line.split("|")[1]
        else:
            continue
        for a in dict.fromkeys(ADDR.findall(cell)):
            if a.upper().startswith("4"):
                yield norm(a), i, section


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", default="main")
    a = ap.parse_args()

    mapped = set()
    for p in MAP:
        mapped.update(norm(x) for x in ADDR.findall((REPO / p).read_text()))
    code = added(a.base, ["tagpu", "tools"])
    missing = {}
    for path, lines in code.items():
        for n, text in lines.items():
            for x in ADDR.findall(text):
                if norm(x) not in mapped:
                    missing.setdefault(norm(x), f"{path}:{n}")
    for addr, where in sorted(missing.items()):
        print(f"MISSING {addr}  {where}")

    new = added(a.base, MAP)
    every = {}
    for p in MAP:
        for addr, n, section in entries(p):
            every.setdefault(addr, []).append((p, n, section))
    extend = 0
    for addr, places in sorted(every.items()):
        fresh = [x for x in places if x[1] in new.get(x[0], {})]
        older = [x for x in places if x not in fresh and x[2] not in {f[2] for f in fresh}]
        if fresh and older:
            extend += 1
            f, o = fresh[0], older[0]
            print(f"EXTEND {addr}  new entry {f[0]}:{f[1]} — it already has one at {o[0]}:{o[1]} "
                  f"({o[2][:80]})")
    print(f"exemap-check: {len(missing)} missing, {extend} to extend", file=sys.stderr)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
