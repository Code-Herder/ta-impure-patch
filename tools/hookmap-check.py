#!/usr/bin/env python3
"""Every engine site the DLL writes must have a row in research/notes/modules/hook-map.md.

A write to engine code goes through tagpu_detour_write, one of the installers built on it
(tagpu_detour.c), or a module's own VirtualProtect; the first argument of each is the site. The site is found from that argument:

  - a literal address, or a #define / const name for one;
  - an array element (NAME[i] or NAME[i].field): every address in NAME's initializer, one per
    row (the first address of each `{ ... }` row, or each element of a flat list);
  - a parameter of the enclosing function: the first argument of that function's callers in
    the same file, resolved by the two rules above.

Anything else (a site computed at run time) is printed as UNRESOLVED for a check by hand; it
is never silently passed.

    tools/hookmap-check.py              sites on lines added since main  (the landing gate)
    tools/hookmap-check.py --base REF   ... since REF
    tools/hookmap-check.py --all        every site in the tree           (the inventory)

Exit 1 if a site is MISSING from the hook map, 0 otherwise. UNRESOLVED lines do not fail the
run: the landing checks each one by hand.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SRC = REPO / "tagpu/ddraw/src"
INC = REPO / "tagpu/ddraw/inc"
HOOKMAP = REPO / "research/notes/modules/hook-map.md"

ADDR = r"0x0*4[0-9A-Fa-f]{5}"
INSTALLER = re.compile(
    r"\b(?:tagpu_detour_(?:write|leaf|leaf_call|gate|observe|land)|patch_bytes|VirtualProtect)"
    r"\s*\(\s*((?:[^,()]|\([^()]*\))+),")
# a parameter declaration (`unsigned int addr`, `const char* s`): two words, no operator
DECL = re.compile(r"\s*(?:const\s+)?[A-Za-z_]\w*(?:\s+[A-Za-z_]\w*)*[\s\*]+\**[A-Za-z_]\w*\s*(?:\[\w*\])?\s*$")
CAST = re.compile(r"\(\s*(?:const\s+)?(?:unsigned(?:\s+(?:int|char))?|int|char|void|size_t|DWORD|"
                  r"uint32_t|BYTE|UINT_PTR|LPVOID)\s*\**\s*\)")


def norm(a: str) -> str:
    return "0x" + a[2:].lstrip("0").upper().rstrip("U")


def strip_comments(t: str) -> str:
    # keep line numbers: a comment's newlines survive
    t = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), t, flags=re.S)
    return re.sub(r"//[^\n]*", "", t)


def constants(t: str) -> dict:
    """Named addresses visible in one source text: #define, enum members and const
    declarations. A plain assignment (`va = 0x...;`) is a variable, not a name for a site."""
    out = {}
    for m in re.finditer(rf"#define\s+(\w+)\s+\(?\s*(?:\(\w+\)\s*)?({ADDR})u?\b", t):
        out[m.group(1)] = m.group(2)
    for e in re.finditer(r"\benum\b[^{;]*\{([^}]*)\}", t):
        for m in re.finditer(rf"\b(\w+)\s*=\s*({ADDR})u?\b", e.group(1)):
            out.setdefault(m.group(1), m.group(2))
    for m in re.finditer(rf"\bconst\s+[\w\s]*?\b(\w+)\s*=\s*({ADDR})u?\s*;", t):
        out.setdefault(m.group(1), m.group(2))
    return out


def header_constants() -> dict:
    out = {}
    for f in INC.glob("tagpu_*.h"):
        out.update(constants(strip_comments(f.read_text(errors="replace"))))
    return out


def line_of(t: str, pos: int) -> int:
    return t.count("\n", 0, pos) + 1


def top_level(body: str, start: int):
    """(text, offset) of each comma-separated element of a brace body, at depth 0."""
    out, depth, cur0 = [], 0, 0
    for i, ch in enumerate(body + ","):
        depth += {"{": 1, "(": 1, "}": -1, ")": -1}.get(ch, 0)
        if ch == "," and depth == 0:
            if body[cur0:i].strip():
                out.append((body[cur0:i], start + cur0))
            cur0 = i + 1
    return out


def initializer(t: str, name: str, consts: dict):
    """(address, line) for each element of NAME's brace initializer in this file: the
    element itself for a flat list, the first address of each row for a table."""
    out = []
    for m in re.finditer(rf"\b{name}\s*(?:\[[^\]]*\]\s*)+=\s*\{{", t):
        i, depth = m.end(), 1
        while i < len(t) and depth:
            depth += {"{": 1, "}": -1}.get(t[i], 0)
            i += 1
        for el, off in top_level(t[m.end():i - 1], m.end()):
            for tok in re.finditer(rf"\b(?:{ADDR}u?|[A-Z_][A-Z0-9_]+)\b", el):
                v = tok.group(0) if tok.group(0).startswith("0") else consts.get(tok.group(0))
                if v:
                    out.append((v, line_of(t, off + tok.start())))
                    break
    return out


def enclosing_function(t: str, pos: int):
    """(name, [params]) of the function definition containing pos."""
    best = None
    for m in re.finditer(r"^[A-Za-z_][\w \*]*?\b(\w+)\s*\(((?:[^()]|\([^()]*\))*)\)\s*\{",
                         t[:pos], flags=re.M):
        best = m
    if not best:
        return None, []
    params = []
    for p, _ in top_level(best.group(2), 0):
        fp = re.search(r"\(\s*[\w\s]*\*\s*(\w+)\s*\)", p)   # void (__cdecl *fn)(void*)
        name = fp.group(1) if fp else (re.findall(r"(\w+)\s*(?:\[\w*\])?\s*$", p) or [""])[0]
        params.append(name)
    return best.group(1), params


def sites():
    shared = header_constants()
    found, unresolved = [], []
    for f in sorted(SRC.glob("tagpu_*.c")):
        if f.name == "tagpu_detour.c":
            continue
        t = strip_comments(f.read_text(errors="replace"))
        rel = f.relative_to(REPO).as_posix()
        consts = {**shared, **constants(t)}

        def caller_args(fn: str, k: int):
            """(k-th argument, position) of every call of fn in this file."""
            for c in re.finditer(rf"\b{fn}\s*\(", t):
                args, d, j, cur = [], 1, c.end(), ""
                while j < len(t) and d:
                    ch = t[j]
                    d += {"(": 1, ")": -1}.get(ch, 0)
                    if (ch == "," and d == 1) or d == 0:
                        args.append(cur)
                        cur = ""
                    else:
                        cur += ch
                    j += 1
                if any(DECL.match(x) for x in args):
                    continue   # the definition or a prototype, not a call
                if len(args) > k:
                    yield args[k], c.start()

        def resolve(arg: str, pos: int, depth: int = 0):
            a = CAST.sub("", arg).strip()
            while re.fullmatch(r"\((.*)\)", a):
                a = a[1:-1].strip()
            if re.fullmatch(rf"{ADDR}u?", a):
                return [(a, line_of(t, pos))]
            if a in consts:
                return [(consts[a], line_of(t, pos))]
            base = re.match(r"&?\s*(\w+)\s*(?:\[|->|\.|$)", a)
            if not base or depth > 3:
                return None
            name = base.group(1)
            rows = initializer(t, name, consts)
            if rows:
                return rows
            fn, params = enclosing_function(t, pos)
            body_from = t.rfind(f"{fn}(", 0, pos) if fn else 0
            # an alias: `o = &sites[i]` or `t = (unsigned char*)(size_t)va` earlier in the function
            al = None
            for al in re.finditer(rf"\b{name}\s*=\s*([^;=]+);", t[body_from:pos]):
                pass
            if al and al.group(1).strip() != a:
                got = resolve(al.group(1), body_from + al.start(), depth + 1)
                if got:
                    return got
            # a parameter: what the callers pass
            if fn and name in params:
                out = []
                for arg2, p2 in caller_args(fn, params.index(name)):
                    out += resolve(arg2, p2, depth + 1) or []
                if out:
                    return out
            # a registry filled at run time: `X->va = va` in a function whose callers pass it
            field = re.search(r"(?:->|\.)\s*(\w+)\s*$", a)
            if field:
                out = []
                for asg in re.finditer(rf"(?:->|\.)\s*{field.group(1)}\s*=\s*(\w+)\s*;", t):
                    if asg.start() == pos:
                        continue
                    out += resolve(asg.group(1), asg.start(), depth + 1) or []
                if out:
                    return out
            return None

        for m in INSTALLER.finditer(t):
            if DECL.match(m.group(1)):
                continue   # an installer's own definition, e.g. patch_bytes
            got = resolve(m.group(1), m.start())
            if got:
                found += [(norm(a), rel, ln) for a, ln in got]
            else:
                unresolved.append((rel, line_of(t, m.start()), m.group(1).strip()))
    return found, unresolved


def added_lines(base: str) -> dict:
    out = {}
    diff = subprocess.run(["git", "-C", str(REPO), "diff", "-U0", f"{base}...HEAD", "--",
                           "tagpu/ddraw/src"], capture_output=True, text=True, check=True).stdout
    cur = None
    for ln in diff.splitlines():
        if ln.startswith("+++ "):
            cur = ln[6:] if ln.startswith("+++ b/") else None
        m = re.match(r"@@ -\S+ \+(\d+)(?:,(\d+))? @@", ln)
        if m and cur:
            s, n = int(m.group(1)), int(m.group(2) or 1)
            out.setdefault(cur, set()).update(range(s, s + n))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", default="main")
    ap.add_argument("--all", action="store_true")
    a = ap.parse_args()
    doc = HOOKMAP.read_text().upper()
    found, unresolved = sites()
    if not a.all:
        added = added_lines(a.base)
        found = [s for s in found if s[2] in added.get(s[1], ())]
        unresolved = [u for u in unresolved if u[1] in added.get(u[0], ())]
    missing = {}
    for addr, f, ln in found:
        if not re.search(rf"\b0X0*{addr[2:]}\b", doc):
            missing.setdefault(addr, f"{f}:{ln}")
    for addr, where in sorted(missing.items()):
        print(f"MISSING {addr}  {where}")
    for f, ln, arg in unresolved:
        print(f"UNRESOLVED {f}:{ln}  {arg}  (check by hand)")
    sites_n = len({s[0] for s in found})
    print(f"hookmap-check: {sites_n} sites, {len(missing)} missing, {len(unresolved)} unresolved",
          file=sys.stderr)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
