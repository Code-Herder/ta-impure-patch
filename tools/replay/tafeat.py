#!/usr/bin/env python3
"""tafeat -- the simulation features of the `sim` files and what a coverage run reached
(inc/tagpu_simreg.h, SIM_FEATURE; inc/tagpu_cover.h). Standard library only: the link recipe runs
`tareplay.py features --check`, which is this module's check().

A FEATURE is every line of a `sim` file from its `SIM_FEATURE(id, puppet, "fixtures")` line to the
file's next one. A FUNCTION is found by a small C parser: a `{` at file scope that follows a `)`
opens one, named by the identifier before that `)`'s `(`; comments, strings, character literals
and preprocessor lines are blanked first, so none of them can open or close a brace. The coverage
build's linker map (ddraw-cover.map) places each function in a `.text$<name>` section of its
object, so a coverage file's addresses name (object, function) pairs, a clone (`.part.N`,
`.isra.N`, `.constprop.N`, `.cold`) under its function's name.
"""
import bisect
import json
import re
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
DDRAW = TREE / "tagpu" / "ddraw"
PUPPETS = ("SIM_PUPPET_RUNS", "SIM_PUPPET_OWNER", "SIM_PUPPET_INDEPENDENT")
FEATURE_RX = re.compile(r'^SIM_FEATURE\((\w+), (SIM_PUPPET_\w+), "([a-z0-9 _-]*)"\);$')
DECL_RX = re.compile(r"^\s*SIM_(VAR|VAR_L|ROWS|ROWS_LIVE|HEAP|BLOCK|ENGINE|TLS|LOCK|CODE|GUARD|"
                     r"TOOL_FILE|PRIVATE)\b")


def sim_files(ddraw=DDRAW):
    """The files thread-split.allow marks `sim`, relative to tagpu/ddraw."""
    out = []
    for line in (Path(ddraw) / "thread-split.allow").read_text().splitlines():
        f = line.split("#", 1)[0].split()
        if len(f) >= 3 and f[2] == "sim":
            out.append(f[0])
    return out


def fixture_names():
    return set(json.loads((HERE / "fixtures.json").read_text())["fixtures"])


# ---- the C parser ---------------------------------------------------------------------------

# a comment, a string or character literal, or a preprocessor line with its continuations; a
# newline inside one is kept, everything else becomes a space
TOKEN_RX = re.compile(r"""/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"?|'(?:\\.|[^'\\\n])*'?|"""
                      r"""(?m:^[ \t]*#(?:\\\n|[^\n])*)""", re.S)


def blank(text):
    """The text with comments, strings, character literals and preprocessor lines (with their
    continuations) turned to spaces, newlines kept, so offsets and line numbers still hold."""
    return TOKEN_RX.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def blank_comments(text):
    """The text with its comments alone turned to spaces, newlines kept: what a landing compares,
    so that a changed #define, string or character literal is a changed line."""
    def gone(t):
        return re.sub(r"[^\n]", " ", t)

    def one(m):
        t = m.group(0)
        if t.startswith(("/*", "//")):
            return gone(t)
        if t.lstrip().startswith("#"):          # a preprocessor line keeps all but its comments
            return re.sub(r"/\*.*?\*/|//[^\n]*", lambda c: gone(c.group(0)), t, flags=re.S)
        return t
    return TOKEN_RX.sub(one, text)


STOCK = "TAGPU_LIMITS_STOCK"
BOTH = frozenset(("raised", "stock"))
# the macros only a test program's command line defines: what they guard is in no DLL build
# (inc/tagpu_recframe.h, RECF_TEST_COPY)
TEST_ONLY = ("RECF_TEST_COPY",)


def builds(text):
    """For each line (1-based list, index 0 unused), the builds that compile it: `raised` (the
    default build), `stock` (make LIMITS=stock), both, or none, from the #if/#ifdef/#ifndef/#else/
    #endif lines that test TAGPU_LIMITS_STOCK and the #ifdef lines of a TEST_ONLY macro; any other
    condition leaves both possible."""
    out = [BOTH]
    stack = []                          # (the branch's builds, the other branch's)
    for line in text.split("\n"):
        d = re.match(r"\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)", line)
        if d:
            kind, rest = d.group(1), d.group(2)
            if kind in ("ifdef", "ifndef", "if"):
                yes = None
                if kind == "ifdef" and (rest.split() or [""])[0] in TEST_ONLY:
                    yes = frozenset()
                elif STOCK in rest and kind == "ifdef":
                    yes = frozenset(("stock",))
                elif STOCK in rest and kind == "ifndef":
                    yes = frozenset(("raised",))
                elif kind == "if" and re.fullmatch(rf"\s*defined\s*\(?\s*{STOCK}\s*\)?\s*(/\*.*)?",
                                                   rest):
                    yes = frozenset(("stock",))
                elif kind == "if" and re.fullmatch(
                        rf"\s*!\s*defined\s*\(?\s*{STOCK}\s*\)?\s*(/\*.*)?", rest):
                    yes = frozenset(("raised",))
                stack.append((BOTH, BOTH) if yes is None else (yes, BOTH - yes))
            elif kind == "elif" and stack:
                stack[-1] = (BOTH, BOTH)
            elif kind == "else" and stack:
                stack[-1] = (stack[-1][1], stack[-1][0])
            elif kind == "endif" and stack:
                stack.pop()
        cur = BOTH
        for b, _ in stack:
            cur = cur & b
        out.append(cur)
    return out


def functions(text):
    """[(name, first line, last line)], 1-based: the first line is the one that holds the name."""
    b = blank(text)
    nl = [i for i, c in enumerate(b) if c == "\n"]

    def line(off):
        return bisect.bisect_right(nl, off - 1) + 1 if off else 1
    out = []
    depth = 0
    opened = None                       # (name offset, name) of the function being read
    for m in re.finditer(r"[{}]", b):
        i = m.start()
        if b[i] == "{":
            if depth == 0:
                j = len(b[:i].rstrip()) - 1
                if j >= 0 and b[j] == ")":
                    pd, k = 0, j
                    while k >= 0:
                        if b[k] == ")":
                            pd += 1
                        elif b[k] == "(":
                            pd -= 1
                            if pd == 0:
                                break
                        k -= 1
                    n = re.search(r"(\w+)\s*$", b[max(0, k - 200):k])
                    opened = (k - 200 + n.start(1) if k >= 200 else n.start(1), n.group(1)) \
                        if n else None
                else:
                    opened = None
            depth += 1
        else:
            depth = max(0, depth - 1)
            if depth == 0 and opened:
                out.append((opened[1], line(opened[0]), line(i)))
                opened = None
    return out


# ---- the features ---------------------------------------------------------------------------

def features_of(path, rel):
    """(features, problems): features as dicts name/file/line/end/puppet/fixtures, in file order."""
    text = Path(path).read_text()
    lines = text.splitlines()
    feats, problems = [], []
    first_code = None
    funcs = functions(text)
    if funcs:
        first_code = funcs[0][1]
    for n, line in enumerate(lines, 1):
        if DECL_RX.match(line) and (first_code is None or n < first_code):
            first_code = n
        if "SIM_FEATURE(" not in line or line.lstrip().startswith(("#define", "/*", "*", "//")):
            continue
        m = FEATURE_RX.match(line)
        if not m:
            problems.append(f"MALFORMED {rel}:{n}: a SIM_FEATURE is one line at the line's start, "
                            f'SIM_FEATURE(id, SIM_PUPPET_..., "fixtures");')
            continue
        name, puppet, fx = m.groups()
        if puppet not in PUPPETS:
            problems.append(f"MALFORMED {rel}:{n}: {puppet} is not one of {', '.join(PUPPETS)}")
        feats.append({"name": name, "file": rel, "line": n, "puppet": puppet,
                      "fixtures": fx.split()})
    for k, f in enumerate(feats):
        f["end"] = feats[k + 1]["line"] - 1 if k + 1 < len(feats) else len(lines)
    if first_code is not None and (not feats or first_code < feats[0]["line"]):
        problems.append(f"UNFEATURED {rel}:{first_code}: code or a declaration before the file's "
                        f"first SIM_FEATURE")
    return feats, problems


def all_features(ddraw=DDRAW):
    feats, problems = [], []
    for rel in sim_files(ddraw):
        f, p = features_of(Path(ddraw) / rel, rel)
        feats += f
        problems += p
    known = fixture_names()
    seen = {}
    for f in feats:
        if f["name"] in seen:
            problems.append(f"DUPLICATE {f['file']}:{f['line']}: feature {f['name']} is also "
                            f"{seen[f['name']]}")
        seen.setdefault(f["name"], f"{f['file']}:{f['line']}")
        for x in f["fixtures"]:
            if x not in known:
                problems.append(f"UNKNOWN {f['file']}:{f['line']}: fixture {x} is not in "
                                f"tools/replay/fixtures.json")
        if not f["fixtures"]:
            problems.append(f"NOFIXTURE {f['file']}:{f['line']}: feature {f['name']} names no "
                            f"fixture")
    return feats, problems


def feature_at(feats, rel, line):
    """The feature of `rel` whose region holds `line`, or None (above the file's first)."""
    hit = None
    for f in feats:
        if f["file"] == rel and f["line"] <= line:
            hit = f
    return hit


def check(ddraw=DDRAW):
    """The link's check: 0 clean, 1 problems (each printed), 2 could not run."""
    try:
        feats, problems = all_features(ddraw)
    except (OSError, ValueError) as e:
        print(f"sim-features: could not run: {e}")
        return 2
    for p in problems:
        print(f"sim-features: {p}")
    if problems:
        return 1
    print(f"sim-features: clean -- {len(feats)} features in {len(sim_files(ddraw))} sim files")
    return 0


# ---- the map and the coverage files ---------------------------------------------------------

def read_map(path):
    """{(source, function): [rva, ...]}, source relative to tagpu/ddraw (src/x.c), one RVA per
    section the function has (clones included)."""
    text = Path(path).read_text(errors="replace")
    m = re.search(r"0x([0-9a-f]+)\s+__image_base__ = ", text)
    if not m:
        raise ValueError(f"{path}: no __image_base__")
    base = int(m.group(1), 16)
    out = {}
    rx = re.compile(r"^ \.text\$(\S+)(?:\s+|\n\s+)0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)$", re.M)
    for name, addr, size, obj in rx.findall(text):
        if not obj.endswith(".o") or int(size, 16) == 0:
            continue
        src = re.sub(r"(\.stock)?(\.cover)?\.o$", ".c", obj)
        fn = name.split(".")[0]
        out.setdefault((src, fn), []).append(int(addr, 16) - base)
    return out


def read_cover(path):
    b = Path(path).read_bytes()
    if len(b) < 8 or b[:4] != b"ICV1":
        raise ValueError(f"{path}: not a coverage file")
    n = struct.unpack_from("<I", b, 4)[0]
    if len(b) != 8 + 4 * n:
        raise ValueError(f"{path}: {n} addresses announced, {(len(b) - 8) // 4} present")
    return set(struct.unpack_from(f"<{n}I", b, 8))


def reached(mapping, covers):
    """{(source, function)} that ran in any of the coverage files."""
    rvas = set()
    for c in covers:
        rvas |= read_cover(c)
    return {k for k, v in mapping.items() if any(r in rvas for r in v)}
