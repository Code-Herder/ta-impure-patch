#!/usr/bin/env python3
"""rules.py -- where a rule of the brain lives, in one call.

    tools/impai/rules.py NAME...            a rule ("THE HOLD"), a function (economy) or a field (hold), any mix
    tools/impai/rules.py --list [FILE]      every rule name with its first site
    tools/impai/rules.py --coupling [N]     the N fields with the most functions reading or writing them

It reads the brain's sources as they are on disk (tagpu/ddraw/src/impai_*.c and impai*.h), so its answer is never
older than the code and there is no index file to keep.

A RULE is a name in capitals the comments use ("THE HOLD", "AT THE RALLY"). Its answer lists every function
whose comment or body names it, the sites where the name opens a sentence first (`*`: where it is most likely
stated), each with the decision fields that function reads and writes.
A FUNCTION's answer is its file and lines, the rules it names, the fields it reads and writes, and the brain
functions it calls and is called by.
A FIELD is a member reached through the decision's pointer (`d->hold`), a flag of the posture as pos.NAME
(`pos.pressed`; `pos` is all of them), or a plan constant as plan.NAME (`plan.retreat_margin`): its answer is who
writes it and who reads it. A field many functions read is where two
rules meet.

The parse is by the sources' own layout, not a C parser: a function's definition starts in column 0 and its
body ends at a `}` in column 0; the comment above a function, up to the previous `}` or `;` in column 0, is
its own. A name the layout hides (a macro's body, a function-like macro) is not listed."""
import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[1] / "tagpu" / "ddraw" / "src"
RULE = re.compile(r"\b(?:THE|AT THE|IN A|NEVER|NO) [A-Z][A-Z'\- ]{2,48}[A-Z]\b")
DEFN = re.compile(r"^(?:static\s+)?[A-Za-z_][\w\s\*]*?\b([a-z_]\w*)\s*\(")
FIELD = re.compile(r"\bd->([a-z_]\w*)((?:\[[^\]]*\])*)\s*(\+\+|--|[-+|&^]?=(?!=))?")
CALL = re.compile(r"(?<![>.\w])([a-z_]\w*)\s*\(")   # a call by name; a member's (ops->economy) is the world's
PLAN = re.compile(r"\bplan->([a-z_]\w*)")          # a plan constant, listed as plan.NAME
# a flag of the posture, listed as pos.NAME; a write of one is a write of `pos` as well
POS = re.compile(r"\bd->pos\.([a-z_]\w*)\s*(\+\+|--|[-+|&^]?=(?!=))?")
COMMENT = re.compile(r"/\*.*?\*/", re.S)


class Fn:
    def __init__(self, file, name, start, end, lead, body):
        self.file, self.name, self.start, self.end = file, name, start, end
        text = lead + body
        code = COMMENT.sub(" ", body)
        self.rules = {}                      # name -> opens a sentence somewhere in this function's comments
        for c in COMMENT.findall(text):
            flat = re.sub(r"\s+", " ", c[2:-2]).strip()
            for m in RULE.finditer(flat):
                before = flat[:m.start()].rstrip()
                opens = (not before or before[-1] in ".:;") and flat[m.end():m.end() + 2].lstrip()[:1] in ":.,"
                self.rules[m.group(0)] = self.rules.get(m.group(0), False) or opens
        self.reads, self.writes = set(), set()
        for m in FIELD.finditer(code):
            (self.writes if m.group(3) else self.reads).add(m.group(1))
        for m in POS.finditer(code):
            (self.writes if m.group(2) else self.reads).add("pos." + m.group(1))
            if m.group(2):
                self.writes.add("pos")
        self.reads |= {"plan." + m.group(1) for m in PLAN.finditer(code)}
        self.calls = {m.group(1) for m in CALL.finditer(code)} - {name}


def parse(path: Path) -> list:
    lines = path.read_text(errors="replace").split("\n")
    bare = COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), "\n".join(lines)).split("\n")
    out, i, last_end = [], 0, 0
    while i < len(lines):
        m = DEFN.match(bare[i])
        if m and not bare[i].startswith(("#", "typedef", "struct", "enum", "union", "return", "if", "for")):
            j = i                      # the signature runs to the first line with a brace or a semicolon
            while j < len(bare) and "{" not in bare[j] and ";" not in bare[j]:
                j += 1
            if j < len(bare) and "{" in bare[j] and ";" not in bare[j].split("{")[0]:
                k = j                  # a body opened and closed on one line is the whole function
                if not (bare[j].rstrip().endswith("}") and bare[j].count("{") == bare[j].count("}")):
                    while k < len(bare) and not bare[k].startswith("}"):
                        k += 1
                lead = "\n".join(lines[last_end:i])
                out.append(Fn(path.name, m.group(1), i + 1, k + 1, lead, "\n".join(lines[i:k + 1])))
                i = last_end = k + 1
                continue
        if bare[i].startswith("}") or (bare[i][:1] not in (" ", "\t", "") and bare[i].rstrip().endswith(";")):
            last_end = i + 1
        i += 1
    return out


def load(src: Path = SRC) -> list:
    fns = []
    for p in sorted(list(src.glob("impai_*.c")) + list(src.glob("impai*.h"))):
        fns += parse(p)
    return fns


def _fields(f: Fn, cap=14) -> str:
    def few(s):
        s = sorted(s)
        return " ".join(s[:cap]) + (f" +{len(s) - cap}" if len(s) > cap else "")
    return f"reads: {few(f.reads - f.writes) or '-'} | writes: {few(f.writes) or '-'}"


def _site(f: Fn) -> str:
    return f"{f.file}:{f.start} {f.name}"


def show_rule(fns, name) -> list:
    hits = [f for f in fns if name in f.rules]
    if not hits:
        return []
    hits.sort(key=lambda f: (not f.rules[name], f.file, f.start))
    out = [f"RULE {name}: named in {len(hits)} functions (* opens a sentence there)"]
    out += [f"  {'*' if f.rules[name] else ' '} {_site(f):<44} {_fields(f, 8)}" for f in hits]
    return out


def show_fn(fns, name) -> list:
    out = []
    names = {f.name for f in fns}
    for f in (f for f in fns if f.name == name):
        callers = sorted(g.name for g in fns if name in g.calls)
        out += [f"FUNCTION {f.name}: {f.file}:{f.start}-{f.end}",
                "  rules: " + (", ".join(sorted(f.rules)) or "-"),
                "  " + _fields(f, 40),
                "  calls: " + (" ".join(sorted(f.calls & names)) or "-"),
                "  called by: " + (" ".join(callers) or "-")]
    return out


def show_field(fns, name) -> list:
    w = [f for f in fns if name in f.writes]
    r = [f for f in fns if name in f.reads and name not in f.writes]
    if not w and not r:
        return []
    return ([f"FIELD {name}: written by {len(w)}, read by {len(r)} others"]
            + [f"  w {_site(f)}" for f in w] + [f"  r {_site(f)}" for f in r])


def listing(fns, file=None) -> list:
    first = {}
    for f in sorted(fns, key=lambda f: (f.file, f.start)):
        if file and f.file != file:
            continue
        for n, opens in f.rules.items():
            if n not in first or (opens and not first[n][1]):
                first[n] = (f, opens)
    return [f"{n:<40} {_site(f)}{'' if opens else '  (named, not stated)'}" for n, (f, opens) in sorted(first.items())]


def coupling(fns, top) -> list:
    use = {}
    for f in fns:
        for n in f.reads | f.writes:
            use.setdefault(n, [set(), set()])[n in f.writes].add(f.name)
    # a field only the entries write is the decision's plumbing (its inputs, its clock), not a rule's flag
    rows = [(n, r, w) for n, (r, w) in use.items() if w and not all(x.endswith("_decide") for x in w)]
    rows.sort(key=lambda t: -len(t[1] - t[2]))
    return [f"{n:<18} written by {len(w):>2}, read by {len(r - w):>3}: writers {' '.join(sorted(w)[:6])}"
            for n, r, w in rows[:top]]


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("names", nargs="*")
    p.add_argument("--list", nargs="?", const="", default=None, metavar="FILE")
    p.add_argument("--coupling", nargs="?", const=25, default=None, type=int, metavar="N")
    p.add_argument("--src", default=str(SRC), help=argparse.SUPPRESS)
    a = p.parse_args(argv)
    fns = load(Path(a.src))
    out, missed = [], []
    if a.list is not None:
        out += listing(fns, a.list or None)
    if a.coupling is not None:
        out += coupling(fns, a.coupling)
    for n in a.names:
        got = show_rule(fns, n) + show_fn(fns, n) + show_field(fns, n)
        if not got and not n.isupper():
            got = show_rule(fns, n.upper())
        if not got:
            missed.append(n)
        out += got
    print("\n".join(out))
    for n in missed:
        print(f"rules: nothing named {n!r}", file=sys.stderr)
    return 1 if missed or not (out or a.names) else 0


if __name__ == "__main__":
    sys.exit(main())
