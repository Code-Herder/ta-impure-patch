#!/usr/bin/env python3
"""Read the frame profiler's samples: where the game thread stood inside a span (tagpu_prof.c,
THE SAMPLER).

    tools/profsample.py <tagpu_prof_samples.bin> [--csv tagpu_prof_game.csv]
    tools/profsample.py <samples> --span units --under 0x48AD30 --top 12

A sample is the span that was open, the instruction pointer, and the stack values that lie in the
exe's image, innermost and outermost. This tool keeps the values that are return addresses (the
bytes before them in the pristine exe are a call) and answers, for a span and the engine function
that runs it (`--under`; by default the unit tick for `units`, the players phase for `players`
and the layers' walker for `particles`): which of that function's own calls was the thread inside. That is the span's STAGE. Under
each stage it lists the functions the instruction pointer was in.

With `--csv`, the game lane's rows of the same run, shares become milliseconds a tick: the span's
measured time times the share. A share is a count of samples, so it carries sampling error: about
one point at 2 % of 10 000 samples, three at 20 % of 200.

Functions are found, not named: the exe's functions start on 16-byte boundaries, and a start is a
boundary that a call targets or that padding or a return precedes. The names below are the engine
map's (research/notes/engine/); an address with no name there is printed as it is."""
import argparse, bisect, collections, csv, struct, subprocess, sys
from pathlib import Path

NAMES = {
    0x48AD30: "unit tick (its own loop)",
    0x471EB0: "particle layers' walker",
    0x437910: "upkeep: speed, direction, wind",
    0x49E1A0: "aiming and firing (AutoAim)",
    0x4B0D60: "scripts (DoScriptsNow)",
    0x489BB0: "damage (the hit)",
    0x41BD10: "repair and regeneration",
    0x43B7C0: "orders, primary controller",
    0x43BAD0: "orders, secondary controller",
    0x43DD20: "movement pass",
    0x48A870: "movement follow-up",
    0x4864B0: "death",
    0x48B710: "network unit sender",
    0x464F80: "players phase (its own loop)",
    0x408C40: "stock AI tick",
    0x40B2C0: "stock AI, second object",
    0x4827B0: "sight emit",
    0x466DC0: "minimap rebuild",
    0x467440: "radar pass",
    0x40EB70: "pathfinder scheduler",
    0x40E630: "pathfinder search",
    0x40E160: "pathfinder bug walk",
    0x40DA70: "pathfinder A* step",
    0x40E050: "pathfinder extraction",
    0x47E890: "bucket walk",
    0x4815A0: "plot cell lookup",
    0x49B090: "projectile collision test",
    0x405FE0: "Standby order (an idle unit's)",
    0x43B700: "idle target search",
    0x40B7B0: "target query on the seat's seen enemies",
    0x40AD80: "candidate search over the seat's seen enemies",
    0x40AA40: "seen-enemies scan",
    0x4089A0: "weapon target scan",
    0x408830: "stock AI planner",
    0x43CD20: "ground step",
    0x43D290: "air step",
    0x43D6D0: "placement after the step",
    0x48A490: "terrain fit (altitude)",
    0x4B0DA0: "script runner (the interpreter)",
    0x4B0C40: "script query, run at once",
    0x4B0BC0: "script query by name",
    0x4B1C00: "script animation stepper",
    0x480770: "script getter callbacks",
    0x4B6CC0: "rotation by a triple",
    0x43E0B0: "piece point (SweetSpot)",
    0x402640: "BuildingBuild order",
    0x49AA80: "can-aim test",
    0x48A1E0: "AutoAim's target read",
    0x4E4400: "64-bit multiply (CRT)",
    0x4EC4C0: "x87 control word (CRT)",
    0x437C90: "composite registry unregister",
    0x440AF0: "pathing map watermark",
    0x495490: "the step",
    0x496790: "the frame function",
}
DISPATCH = {"units": 0x48AD30, "players": 0x464F80, "particles": 0x471EB0}
STEP = 0x495490


class Exe:
    def __init__(self, path):
        d = Path(path).read_bytes()
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        opt = struct.unpack_from("<H", d, pe + 20)[0]
        self.base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
        self.lo = self.hi = 0
        for i in range(nsec):
            o = pe + 24 + opt + i * 40
            name = d[o:o + 8].rstrip(b"\0")
            vsize, va, rsize, raw = struct.unpack_from("<IIII", d, o + 8)
            if name == b".text":
                self.lo, self.hi = self.base + va, self.base + va + min(vsize, rsize)
                self.b = d[raw:raw + min(vsize, rsize)]
        if not self.lo:
            raise SystemExit(f"{path}: no .text section")
        self.starts = self._starts()

    def byte(self, va):
        return self.b[va - self.lo]

    def _starts(self):
        b, lo, hi = self.b, self.lo, self.hi
        targets = set()
        i = b.find(b"\xE8")
        while i != -1 and i + 5 <= len(b):
            t = lo + i + 5 + struct.unpack_from("<i", b, i + 1)[0]
            if lo <= t < hi and not t & 15:
                targets.add(t)
            i = b.find(b"\xE8", i + 1)
        first = (lo + 15) & ~15
        for a in range(first, hi, 16):
            o = a - lo
            if o >= 3 and (b[o - 1] in (0x90, 0xC3, 0xCC) or b[o - 3] == 0xC2):
                targets.add(a)
        targets.add(lo)
        return sorted(targets)

    def func(self, va):
        """the start of the function `va` is in, or None outside .text"""
        if not self.lo <= va < self.hi:
            return None
        return self.starts[bisect.bisect_right(self.starts, va) - 1]

    def is_ret(self, v):
        """`v` follows a call instruction"""
        if not self.lo + 7 <= v < self.hi:
            return False
        g = self.byte
        if g(v - 5) == 0xE8:
            return True
        if g(v - 6) == 0xFF and (g(v - 5) == 0x15 or g(v - 5) & 0xF8 == 0x90):
            return True
        if g(v - 2) == 0xFF and g(v - 1) & 0x38 == 0x10 and g(v - 1) & 0xC0 in (0xC0, 0x00):
            return True
        if g(v - 3) == 0xFF and g(v - 2) & 0x38 == 0x10 and (g(v - 2) & 0xC0 == 0x40 or g(v - 2) == 0x14):
            return True
        if g(v - 4) == 0xFF and g(v - 3) == 0x54:
            return True
        return g(v - 7) == 0xFF and g(v - 6) == 0x94

    def callee(self, v):
        """the target of the direct call that returns to `v`, or None for an indirect one"""
        if self.byte(v - 5) == 0xE8:
            return v + struct.unpack_from("<i", self.b, v - 4 - self.lo)[0]
        return None


def label(a):
    if a is None:
        return "?"
    return NAMES.get(a, f"0x{a:06X}")


def read(path):
    d = Path(path).read_bytes()
    nl = d.index(b"\n")
    head = dict(kv.split("=", 1) for kv in d[:nl].decode().split()[1:])
    size, kin, kout = int(head["record"]), int(head["in"]), int(head["out"])
    rec = struct.Struct(f"<IIIBBBB{kin}I{kout}I")
    if size != rec.size:
        raise SystemExit(f"{path}: records of {size} bytes, its header describes {rec.size}")
    spans = head["spans"].split(",")
    dll_lo, dll_len = (int(x, 16) for x in head["dll"].split("+"))
    out = []
    for o in range(nl + 1, len(d) - size + 1, size):
        r = rec.unpack_from(d, o)
        frame, eip, esp, span, nin, nout, flags = r[:7]
        # innermost first, then the outermost kept, nearest the stack pointer first
        chain = list(r[7:7 + nin]) + list(r[7 + kin:7 + kin + nout])[::-1]
        out.append((frame, eip, span, flags, chain))
    return spans, (dll_lo, dll_lo + dll_len), out


class Dll:
    """Impure's own functions, from the symbols of an unstripped build of the DLL that ran"""
    def __init__(self, path, loaded_at):
        self.at, self.names = [], []
        if not path:
            return
        tool = next((t for t in ("i686-w64-mingw32-nm", "nm") if subprocess.run(
            ["sh", "-c", f"command -v {t}"], capture_output=True).returncode == 0), None)
        hdr = Path(path).read_bytes()[:4096]
        pe = struct.unpack_from("<I", hdr, 0x3C)[0]
        base = struct.unpack_from("<I", hdr, pe + 24 + 28)[0]
        out = subprocess.run([tool, "-n", str(path)], capture_output=True, text=True).stdout if tool else ""
        for line in out.splitlines():
            p = line.split()
            if len(p) == 3 and p[1] in "tT":
                self.at.append(int(p[0], 16) - base + loaded_at)
                self.names.append(p[2].lstrip("_"))

    def func(self, va):
        i = bisect.bisect_right(self.at, va) - 1
        return self.names[i] if i >= 0 else None


def where(exe, dll, eip):
    f = exe.func(eip)
    if f is not None:
        return label(f)
    if dll[0] <= eip < dll[1]:
        name = dll[2].func(eip)
        return f"Impure: {name}" if name else "Impure (ddraw.dll)"
    return "system and other modules"


def per_tick(csv_path):
    """{span: ms a tick} from the game lane's rows"""
    with open(csv_path, newline="") as f:
        rows = list(csv.reader(f))
    head, rows = rows[0], [r for r in rows[2:] if len(r) == len(rows[0])]
    ticks = sum(int(r[3]) for r in rows)
    if not ticks:
        return {}
    return {name: sum(int(r[5 + k]) for r in rows) / 1000.0 / ticks for k, name in enumerate(head[5:])}


def report(exe, dll, spans, samples, span, under, end, top, ms):
    k = spans.index(span)
    mine = [s for s in samples if s[2] == k]
    if not mine:
        print(f"{span}: no samples")
        return
    stages = collections.Counter()
    leaves = collections.defaultdict(collections.Counter)
    inside = (lambda v: under <= v < end) if end else (lambda v: exe.func(v) == under)
    for frame, eip, _, flags, chain in mine:
        stage = None
        rets = [v for v in reversed(chain) if exe.is_ret(v)]          # outermost first
        for v in rets:
            if inside(v):
                c = exe.callee(v)
                stage = label(c) if c is not None else f"indirect call at 0x{v:06X}"
                break
        if stage is None and under not in DISPATCH.values():
            stage = label(under) + " itself" if inside(eip) else "elsewhere in the span"
        if stage is None:
            # No return into the function itself: Impure took that call site with a jump, and
            # its stub made the call. The stage is then the first engine function under the
            # step that is not this one: the callee, seen from a call of its own.
            funcs = [exe.func(v) for v in rets]
            if STEP in funcs:
                funcs = funcs[len(funcs) - funcs[::-1].index(STEP):]
            inner = next((f for f in funcs if f not in (under, STEP)), None)
            if inner is None:
                inner = exe.func(eip)                # a callee that has called nothing itself
            stage = label(inner) if inner is not None else "no engine frame under it: " + where(exe, dll, eip)
        stages[stage] += 1
        leaves[stage][where(exe, dll, eip)] += 1
    n = len(mine)
    scale = ms.get(span)
    print(f"{span}: {n} samples under {label(under)} 0x{under:06X}"
          + (f", {scale:.2f} ms a tick" if scale else ""))
    rest = 0
    for stage, c in stages.most_common():
        if c * 200 < n:                              # under half a percent of the span
            rest += c
            continue
        line = f"  {c * 100.0 / n:5.1f}%"
        if scale:
            line += f" {scale * c / n:7.2f} ms"
        print(f"{line}  {stage}")
        for leaf, lc in leaves[stage].most_common(top):
            if lc * 200 < n:                         # under half a percent of the span
                continue
            print(f"         {lc * 100.0 / n:5.1f}%    in {leaf}")
    if rest:
        print(f"  {rest * 100.0 / n:5.1f}%" + (f" {scale * rest / n:7.2f} ms" if scale else "")
              + "  stages under half a percent each")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("samples")
    ap.add_argument("--exe", help="the pristine exe (default: pristine/TotalA.exe.pristine of the main checkout)")
    ap.add_argument("--csv", help="the game lane's rows of the same run, for milliseconds a tick")
    ap.add_argument("--dll", help="the ddraw.dll that ran, linked unstripped, to name Impure's own functions")
    ap.add_argument("--span", action="append", help="a span to report (default: units and players)")
    ap.add_argument("--under", help="the function whose calls are the stages: its address, or START:END "
                    "for one the start heuristic cuts short (a long body with aligned inner labels)")
    ap.add_argument("--top", type=int, default=6, help="functions listed under a stage")
    a = ap.parse_args()
    exe_path = a.exe
    if not exe_path:
        common = subprocess.run(["git", "rev-parse", "--git-common-dir"], capture_output=True, text=True,
                                cwd=Path(__file__).resolve().parent).stdout.strip()
        exe_path = (Path(__file__).resolve().parent / common).resolve().parent / "pristine" / "TotalA.exe.pristine"
    exe = Exe(exe_path)
    spans, dll, samples = read(a.samples)
    dll = (dll[0], dll[1], Dll(a.dll, dll[0]))
    ms = per_tick(a.csv) if a.csv else {}
    total = len(samples)
    by = collections.Counter(s[2] for s in samples)
    unread = sum(1 for s in samples if s[3] & 1)
    print(f"{a.samples}: {total} samples, {unread} with the stack not read")
    print("  by span: " + ", ".join(f"{spans[k]} {c * 100.0 / total:.1f}%" for k, c in by.most_common() if c * 200 >= total))
    for span in a.span or list(DISPATCH):
        lo, _, hi = (a.under or "").partition(":")
        under = int(lo, 16) if lo else DISPATCH.get(span)
        if under is None:
            raise SystemExit(f"--span {span} needs --under")
        report(exe, dll, spans, samples, span, under if hi else exe.func(under), int(hi, 16) if hi else 0, a.top, ms)
    return 0


if __name__ == "__main__":
    sys.exit(main())
