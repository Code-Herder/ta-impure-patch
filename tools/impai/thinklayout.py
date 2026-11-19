#!/usr/bin/env python3
"""thinklayout.py -- the layout of ImpAI's recorded state, as the DLL's compiler lays it out.

    tools/impai/thinklayout.py [--tree DIR] [--part mil|brain|job|known]     print a region's fields

think.py reads the brain's state out of a recording by field name. The offsets, sizes and types of those fields
are not written anywhere in Python: they are the compiler's. A probe that includes `impai.h` and declares one
variable of each recorded region's type is compiled with the DLL's compiler and flags (the Makefile's `CC` and
`CFLAGS`, read from its text: running make would write the tree's generated headers), with debug information
for every type and macro of the header, and `objdump --dwarf` gives each struct's members, each array's bound,
each base type's size and signedness, every enumerator and every macro that is a plain number. Nothing is
executed, so the target's 32-bit Windows ABI needs no emulator.

THE REGIONS are the four think.py reads, each a table of rows of one C type, the type read from the region's
definition in tagpu_impai.c (`static IMPAI_MIL s_mil[...]`): s_mil and s_brain a row a seat, s_job and s_known a
row a unit slot.

THE CACHE. A layout is a function of the header and what it includes, the compiler and its flags, and the
regions' types, so it is kept under the SHA-256 of all of those (key): a tree whose headers change has another
key, and a stale entry cannot be read for it. An entry that does not parse is made again.

A layout is a dict, JSON as kept: `types` {id: node}, `roots` {region: id}, `enums` [{name: value}] one a C enum,
`macros` {name: value}, and what made it (`cc`, `flags`, `headers`). A node is one of
    {"k": "u" | "i" | "f" | "b" | "p", "size": n}      unsigned, signed, float, boolean, pointer
    {"k": "e", "size": n, "signed": 0 | 1, "names": {value: name}}
    {"k": "s", "name": tag, "size": n, "m": [[member, offset, id], ...]}      a struct or a union
    {"k": "a", "n": count, "e": id, "size": n}
    {"k": "x", "why": text}                            what no field can be read as (a bit-field)"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
DDRAW = Path("tagpu") / "ddraw"
HEADER = DDRAW / "src" / "impai.h"
SOURCE = DDRAW / "src" / "tagpu_impai.c"
MAKEFILE = DDRAW / "Makefile"
REGIONS = {"mil": "s_mil", "brain": "s_brain", "job": "s_job", "known": "s_known"}
FORMAT = 1                      # of the kept layout: a change here makes every entry anew
# what the probe adds to the DLL's flags: its own include folder, every type and macro in the debug
# information whether the probe uses it or not, and DWARF 4, whose member offsets are plain numbers
PROBE_FLAGS = ["-Isrc", "-g3", "-gdwarf-4", "-fno-eliminate-unused-debug-types", "-c"]
DIE = re.compile(r"^ <(\d+)><([0-9a-f]+)>: Abbrev Number: \d+ \((DW_TAG_\w+)\)")
ATTR = re.compile(r"^\s+<[0-9a-f]+>\s+(DW_AT_\w+)\s*: (.*)$")
MACRO = re.compile(r"DW_MACRO_define(?:_strp)? - lineno : \d+ macro : (\w+) (\S+)\s*$")
NUMBER = re.compile(r"^\(?(-?(?:0[xX][0-9a-fA-F]+|\d+))[uUlL]*\)?$")
ENCODING = {2: "b", 4: "f", 5: "i", 6: "i", 7: "u", 8: "u"}
SKIPPED = ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type")


class LayoutError(Exception):
    pass


def region_types(tree: Path) -> dict:
    """{region: its row's C type}, from each region's definition in tagpu_impai.c."""
    try:
        text = (tree / SOURCE).read_text(errors="replace")
    except OSError as e:
        raise LayoutError(f"{tree} is no source tree of the DLL: no {SOURCE} ({e.strerror})") from None
    out = {}
    for part, name in REGIONS.items():
        m = re.search(rf"^static\s+(\w+)\s+{name}\s*\[", text, re.M)
        if not m:
            raise LayoutError(f"{tree / SOURCE} defines no region {name}")
        out[part] = m.group(1)
    return out


def headers(tree: Path) -> list:
    """impai.h and every header it includes by a quoted name, transitively, each found beside the header that
    names it or in the DLL's inc/: the files whose text decides the layout."""
    todo, seen = [tree / HEADER], []
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        try:
            text = f.read_text(errors="replace")
        except OSError as e:
            raise LayoutError(f"no {f}: {e.strerror}") from None
        seen.append(f)
        for name in re.findall(r'^\s*#\s*include\s+"([^"]+)"', text, re.M):
            for folder in (f.parent, tree / DDRAW / "inc"):
                if (folder / name).is_file():
                    todo.append(folder / name)
                    break
    return sorted(seen)


def compiler(tree: Path) -> tuple:
    """(the DLL's compiler, its flags), from the Makefile's text."""
    try:
        mk = (tree / MAKEFILE).read_text(errors="replace")
    except OSError as e:
        raise LayoutError(f"no {tree / MAKEFILE}: {e.strerror}") from None
    cc = re.search(r"^CC\s*[:?]?=\s*(\S+)", mk, re.M)
    flags = re.search(r"^CFLAGS\s*[:?]?=\s*(.+)$", mk, re.M)
    if not cc or not flags:
        raise LayoutError(f"{tree / MAKEFILE} names no CC or no CFLAGS")
    return cc.group(1), flags.group(1).split()


def cc_version(cc: str) -> str:
    try:
        r = subprocess.run([cc, "--version"], capture_output=True, text=True)
    except OSError as e:
        raise LayoutError(f"the DLL's compiler {cc} does not run: {e.strerror}") from None
    return (r.stdout.splitlines() or [""])[0]


def key(tree: Path) -> str:
    """THE CACHE's key for `tree`."""
    if not (tree / HEADER).is_file():
        raise LayoutError(f"{tree} is no source tree of the DLL: it has no {HEADER}")
    cc, flags = compiler(tree)
    h = hashlib.sha256(json.dumps([FORMAT, cc, cc_version(cc), flags, PROBE_FLAGS,
                                   sorted(region_types(tree).items())]).encode())
    for f in headers(tree):
        h.update(f.relative_to(tree).as_posix().encode() + b"\0" + f.read_bytes() + b"\0")
    return h.hexdigest()


def dies(text: str) -> dict:
    """objdump's .debug_info listing as {offset: {"tag", "kids": [offset], attribute: text}}."""
    out, stack = {}, []
    cur = None
    for ln in text.splitlines():
        m = DIE.match(ln)
        if m:
            depth, off = int(m.group(1)), m.group(2)
            cur = out[off] = {"tag": m.group(3), "kids": []}
            del stack[depth:]
            if stack:
                out[stack[-1]]["kids"].append(off)
            stack.append(off)
            continue
        m = ATTR.match(ln)
        if m and cur is not None:
            cur[m.group(1)] = m.group(2).strip()
    return out


def _name(d: dict):
    n = d.get("DW_AT_name")
    return n.rsplit("): ", 1)[-1] if n is not None and n.startswith("(indirect") else n


def _int(d: dict, attr: str, what: str) -> int:
    try:
        return int(d[attr].split()[0], 0)
    except (KeyError, ValueError, IndexError):
        raise LayoutError(f"{what}: no plain {attr} in the debug information "
                          f"({d.get(attr, 'absent')!r})") from None


def _ref(d: dict):
    t = d.get("DW_AT_type")
    return t.strip("<>")[2:] if t else None


def build(info: str, macro: str, roots: dict) -> dict:
    """The layout from objdump's two listings; `roots` {region: the probe's variable of its type}."""
    ds = dies(info)
    types = {}

    def node(off):
        d = ds.get(off)
        while d is not None and d["tag"] in SKIPPED:
            off = _ref(d)
            d = ds.get(off)
        if d is None:
            raise LayoutError(f"the debug information names a type <{off}> it does not hold")
        if off in types:
            return off
        tag = d["tag"]
        if tag == "DW_TAG_base_type":
            enc = _int(d, "DW_AT_encoding", "a base type")
            if enc not in ENCODING:
                raise LayoutError(f"a base type of encoding {d['DW_AT_encoding']}: none think.py reads")
            types[off] = {"k": ENCODING[enc], "size": _int(d, "DW_AT_byte_size", "a base type")}
        elif tag == "DW_TAG_enumeration_type":
            names = {}
            for k in d["kids"]:
                names.setdefault(str(_int(ds[k], "DW_AT_const_value", "an enumerator")), _name(ds[k]))
            types[off] = {"k": "e", "size": _int(d, "DW_AT_byte_size", "an enum"),
                          "signed": int(_int(d, "DW_AT_encoding", "an enum") in (5, 6)), "names": names}
        elif tag == "DW_TAG_pointer_type":
            types[off] = {"k": "p", "size": _int(d, "DW_AT_byte_size", "a pointer")}
        elif tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
            me = types[off] = {"k": "s", "name": _name(d) or "", "size": _int(d, "DW_AT_byte_size", "a struct"),
                               "m": []}
            for k in d["kids"]:
                m = ds[k]
                if m["tag"] != "DW_TAG_member":
                    continue
                what = f"{me['name']}.{_name(m)}"
                if "DW_AT_bit_size" in m or "DW_AT_data_bit_offset" in m:
                    types[k] = {"k": "x", "why": "a bit-field"}
                    me["m"].append([_name(m), 0, k])
                    continue
                at = _int(m, "DW_AT_data_member_location", what) if "DW_AT_data_member_location" in m else 0
                me["m"].append([_name(m), at, node(_ref(m))])
        elif tag == "DW_TAG_array_type":
            elem = node(_ref(d))
            for k in reversed([k for k in d["kids"] if ds[k]["tag"] == "DW_TAG_subrange_type"]):
                n = _int(ds[k], "DW_AT_upper_bound", "an array") + 1 if "DW_AT_upper_bound" in ds[k] else 0
                wrap = off if k == d["kids"][0] else f"{off}.{k}"
                types[wrap] = {"k": "a", "n": n, "e": elem, "size": n * types[elem]["size"]}
                elem = wrap
            if elem != off:
                raise LayoutError("an array with no bound in the debug information")
        else:
            raise LayoutError(f"a {tag} in a recorded struct: none think.py reads")
        return off

    variables = {_name(d): d for d in ds.values() if d["tag"] == "DW_TAG_variable"}
    out_roots = {}
    for part, var in roots.items():
        if var not in variables:
            raise LayoutError(f"the probe's {var} is not in its debug information")
        out_roots[part] = node(_ref(variables[var]))
    enums = [{_name(ds[k]): _int(ds[k], "DW_AT_const_value", "an enumerator") for k in d["kids"]}
             for d in ds.values() if d["tag"] == "DW_TAG_enumeration_type"]
    macros = {}
    for ln in macro.splitlines():
        m = MACRO.search(ln)
        n = NUMBER.match(m.group(2)) if m else None
        if n:
            macros[m.group(1)] = int(n.group(1), 0)
    return {"format": FORMAT, "types": types, "roots": out_roots, "enums": enums, "macros": macros}


def generate(tree: Path) -> dict:
    """`tree`'s layout, made now: the probe compiled, its debug information read."""
    if not (tree / HEADER).is_file():
        raise LayoutError(f"{tree} is no source tree of the DLL: it has no {HEADER}")
    cc, flags = compiler(tree)
    rtypes = region_types(tree)
    objdump = cc[:-len("gcc")] + "objdump" if cc.endswith("gcc") else "objdump"
    with tempfile.TemporaryDirectory(prefix="thinklayout-") as tmp:
        src, obj = Path(tmp) / "probe.c", Path(tmp) / "probe.o"
        src.write_text(f'#include "{HEADER.name}"\n'
                       + "".join(f"{t} think_{part};\n" for part, t in rtypes.items()))
        try:
            # in the DLL's own folder, as make runs the compiler, so its relative -I flags mean the same
            r = subprocess.run([cc, *flags, *PROBE_FLAGS, str(src), "-o", str(obj)], cwd=tree / DDRAW,
                               capture_output=True, text=True)
            if r.returncode != 0:
                errors = [ln for ln in r.stderr.splitlines() if "error" in ln][:5]
                raise LayoutError(f"{tree}: the layout probe does not compile: "
                                  + "; ".join(errors or ["no reason given"]))
            info = subprocess.run([objdump, "--dwarf=info", str(obj)], capture_output=True, text=True)
            macro = subprocess.run([objdump, "--dwarf=macro", str(obj)], capture_output=True, text=True)
        except OSError as e:
            raise LayoutError(f"{e.filename or cc} does not run: {e.strerror}") from None
        if info.returncode != 0 or macro.returncode != 0:
            raise LayoutError(f"{objdump} could not read the probe's debug information")
    lay = build(info.stdout, macro.stdout, {part: f"think_{part}" for part in rtypes})
    lay.update(cc=cc_version(cc), flags=flags, ctypes=rtypes,
               headers=[f.relative_to(tree).as_posix() for f in headers(tree)])
    return lay


def load(tree: Path, cache: Path = None) -> dict:
    """`tree`'s layout, from THE CACHE under `cache` when it holds it (no cache: made each call)."""
    tree = Path(tree)
    if cache is None:
        return generate(tree)
    f = Path(cache) / f"layout-{key(tree)[:24]}.json"
    try:
        lay = json.loads(f.read_text())
        if lay.get("format") == FORMAT and set(lay["roots"]) == set(REGIONS):
            return lay
    except (OSError, ValueError, KeyError, AttributeError):
        pass
    lay = generate(tree)
    try:
        f.parent.mkdir(parents=True, exist_ok=True)
        part = f.with_name(f"{f.name}.{os.getpid()}.part")
        part.write_text(json.dumps(lay))
        os.replace(part, f)
    except OSError:
        pass                    # a cache that cannot be written costs the next call a compile, no more
    return lay


def leaves(lay: dict, part: str) -> list:
    """Every scalar of region `part`'s row, in offset order: (name as C names it from the row, offset, node,
    (the struct it is a member of, the member))."""
    out = []

    def walk(tid, name, at, owner):
        n = lay["types"][tid]
        if n["k"] == "s":
            for member, off, mt in n["m"]:
                walk(mt, f"{name}.{member}" if name and member else name or member or "", at + off,
                     (n["name"], member))
        elif n["k"] == "a":
            esize = lay["types"][n["e"]]["size"]
            for i in range(n["n"]):
                walk(n["e"], f"{name}[{i}]", at + i * esize, owner)
        else:
            out.append((name, at, n, owner))
    walk(lay["roots"][part], "", 0, ("", ""))
    out.sort(key=lambda leaf: leaf[1])
    return out


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--tree", type=Path, default=TREE, help="the source tree (default: this one)")
    p.add_argument("--part", choices=sorted(REGIONS), default=None)
    a = p.parse_args(argv)
    try:
        lay = generate(a.tree)
    except LayoutError as e:
        print(f"thinklayout: COULD NOT RUN: {e}")
        return 2
    for part in ([a.part] if a.part else REGIONS):
        root = lay["types"][lay["roots"][part]]
        ls = leaves(lay, part)
        print(f"{part}: {REGIONS[part]}, rows of {lay['ctypes'][part]}, {root['size']} bytes, {len(ls)} scalars")
        if a.part:
            for name, at, n, _owner in ls:
                print(f"  +{at:#06x} {n['k']}{n.get('size', '')} {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
