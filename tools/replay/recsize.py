#!/usr/bin/env python3
"""recsize.py -- where a demo recording's disk space goes, by category.

    recsize.py <file.imprec> [--json out.json] [--window 900]

Reads a recording with tarec.py's block reader and reports:

  - the file by block kind, raw and packed;
  - the stream and the snapshots by record category, raw, and each category deflated alone
    with the writer's settings (lodepng's default: a 2,048-byte window), whose sum is checked
    against what the file really holds;
  - a unit delta's changed dwords, by field, with the changed units per iteration against the
    live ones, and how many bits each top field's change needs;
  - the same for projectiles and the globals, the order lists re-sent and what changed in them,
    and the feature-grid cells by where they were written;
  - bytes per window of game time (the battle against the quiet);
  - what other compressors make of the same blocks.

The packed bytes of a category are an attribution, not a measurement: each block's records
of one category are deflated together, apart from the rest. The check line says how far the
sum is from the file's real packed bytes.
"""
import argparse
import collections
import json
import lzma
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tarec  # noqa: E402

try:
    import zstandard
except ImportError:            # the shared venv has it; the system python does not
    zstandard = None

T = tarec
GROUPS = {
    "units: whole images": [T.RK_UNIT],
    "units: deltas": [T.RK_UNIT_DELTA],
    "units: gone": [T.RK_UNIT_GONE],
    "order lists": [T.RK_ORDERS],
    "players": [T.RK_PLAYER, T.RK_PLAYER_DELTA],
    "tables": [T.RK_TABLE_SIZE, T.RK_TABLE_SPAN],
    "globals": [T.RK_GLOBALS, T.RK_GLOBALS_DELTA],
    "projectiles": [T.RK_PROJ_HEAD, T.RK_PROJ, T.RK_PROJ_DELTA],
    "feature grid cells": [T.RK_CELLS],
    "wrecks and feature heads": [T.RK_WRECK, T.RK_FEATURES, T.RK_GRID],
    "explored map": [T.RK_MAPPED, T.RK_MAPPED_RUNS],
    "sight": [T.RK_SIGHT],
    "events": sorted(T.EVENTS),
    "self-verification": [T.RK_VERIFY_UNIT, T.RK_VERIFY_PLAYER, T.RK_VERIFY_TABLE,
                          T.RK_VERIFY_ORDERS, T.RK_VERIFY_GLOBALS, T.RK_VERIFY_PROJ],
    "perspective": [T.RK_POV, T.RK_CLICK, T.RK_SELECT, T.RK_POV_AT],
    "iteration marks": [T.RK_TICK],
    "effect counts": [T.RK_COUNTS, T.RK_COUNT_NAME],
}
GROUP_OF = {k: g for g, ks in GROUPS.items() for k in ks}
KIND_NAME = {v: n[3:].lower() for n, v in vars(T).items()
             if n.startswith("RK_") and isinstance(v, int) and n not in ("RK_NAMES",)}

UNIT_FIELDS = {     # unit record offsets (the U_ names of tagpu_replay.c and replay-layout.tsv)
    0x00: "mover ptr", 0x04: "weapon1", 0x20: "weapon2", 0x3C: "weapon3", 0x5C: "orders",
    0x60: "suborders", 0x64: "turn", 0x6A: "position", 0x6E: "altitude", 0x76: "footprint x",
    0x78: "footprint z", 0x7A: "sight xz", 0x7E: "footprint w", 0x80: "footprint h",
    0x82: "sort bucket", 0x86: "carrier", 0x8A: "cargo", 0x8E: "next", 0x92: "type",
    0x96: "owner", 0x9A: "cob", 0x9E: "model", 0xA6: "type index", 0xAC: "group",
    0xF0: "attacker", 0xF8: "sight h", 0xF9: "piece", 0xFF: "owner byte", 0x104: "build fraction",
    0x108: "hp", 0x10A: "water", 0x10E: "state byte", 0x10F: "yard", 0x110: "state bits",
}


def field_names():
    """Each unit image dword's name: the layout file's rows and the known offsets above, the
    mover's after the unit record (REC_MOVER_AT = 0x118)."""
    names = {}
    layout = Path(__file__).resolve().parents[2] / "tagpu/ddraw/replay-layout.tsv"
    rows = []
    for line in layout.read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        c = line.split("\t")
        if c[0] in ("unit", "mover") and len(c) > 5:
            base = 0x118 if c[0] == "mover" else 0
            rows.append((base + int(c[1], 16), int(c[2]), c[5]))
    rows += [(o, 4, n) for o, n in UNIT_FIELDS.items()]
    for d in range(T.REC_UNIT_DWORDS):
        lo, hi = 4 * d, 4 * d + 4
        hit = sorted({n for o, ln, n in rows if o < hi and o + ln > lo})
        names[d] = f"+0x{lo:03X} " + ("/".join(hit) if hit else ("mover" if lo >= 0x118 else "?"))
    return names


def deflate_like_writer(data):
    """lodepng's default settings: dynamic Huffman, LZ77 with a 2,048-byte window."""
    c = zlib.compressobj(6, zlib.DEFLATED, 11)
    return len(c.compress(data) + c.flush())


def bitlen(v):
    """The bits a signed change needs."""
    return (v if v >= 0 else ~v).bit_length() + 1


class Delta:
    """Changed dwords of a delta record kind: counts per dword, and the changes' sizes."""

    def __init__(self, ndwords, nbits, head):
        self.ndwords, self.nbits, self.head = ndwords, nbits, head
        self.changed = collections.Counter()
        self.bits = collections.defaultdict(collections.Counter)
        self.last = {}
        self.records = 0
        self.overhead = 0

    def full(self, key, image):
        self.last[key] = list(struct.unpack_from(f"<{len(image) // 4}I", image))

    def delta(self, key, bitmap, words):
        self.records += 1
        self.overhead += 4 + self.head          # record header + index + bitmap, padded
        prev = self.last.get(key)
        k = 0
        for d in range(self.ndwords):
            if bitmap[d >> 3] >> (d & 7) & 1:
                v = struct.unpack_from("<I", words, 4 * k)[0]
                k += 1
                self.changed[d] += 1
                if prev is not None:
                    diff = (v - prev[d] + 0x80000000) % 0x100000000 - 0x80000000
                    self.bits[d][min(bitlen(diff), 33)] += 1
                    prev[d] = v


def analyse(path, window):
    sizes = []
    bl, stop = T.blocks(path, sizes)
    if stop:
        print(f"note: the reading stopped early: {stop}")
    h = T.header(bl[0][3])
    names = field_names()
    file_bytes = Path(path).stat().st_size
    out = {"file": str(path), "bytes": file_bytes, "map": h.get("map"),
           "slots": h.get("slots"), "players": len(h.get("players", []))}

    by_block = collections.defaultdict(lambda: [0, 0, 0])
    for kind, first, last, raw, packed in sizes:
        b = by_block[T.RB_NAMES.get(kind, kind)]
        b[0] += 1; b[1] += raw; b[2] += packed + 32
    out["blocks"] = {k: {"n": v[0], "raw": v[1], "on_disk": v[2]} for k, v in by_block.items()}

    def scan(which):
        raw = collections.Counter()
        alone = collections.Counter()
        nrec = collections.Counter()
        kraw = collections.Counter()
        knum = collections.Counter()
        real = 0
        recomp = 0
        for (kind, first, last, payload), (_, _, _, r, pk) in zip(bl, sizes):
            if kind != which:
                continue
            real += pk
            recomp += deflate_like_writer(payload)
            parts = collections.defaultdict(bytearray)
            for rk, body in T.records(payload):
                n = 4 + ((len(body) + 3) & ~3)
                g = GROUP_OF.get(rk, "other")
                raw[g] += n; nrec[g] += 1
                kraw[rk] += n; knum[rk] += 1
                parts[g] += struct.pack("<HH", rk, len(body)) + body + bytes(-len(body) % 4)
            for g, data in parts.items():
                alone[g] += deflate_like_writer(bytes(data))
        return raw, alone, nrec, kraw, knum, real, recomp

    report = {}
    for which, label in ((T.RB_STREAM, "stream"), (T.RB_SNAPSHOT, "snapshots")):
        raw, alone, nrec, kraw, knum, real, recomp = scan(which)
        report[label] = {"real_packed": real, "recompressed": recomp,
                         "sum_alone": sum(alone.values()),
                         "groups": {g: {"records": nrec[g], "raw": raw[g], "packed_alone": alone[g]}
                                    for g in sorted(raw, key=lambda g: -alone[g])},
                         "kinds": {KIND_NAME.get(k, str(k)): {"records": knum[k], "raw": kraw[k]}
                                   for k in sorted(kraw, key=lambda k: -kraw[k])}}
    out.update(report)

    # the stream record by record: deltas, orders, cells, live units, the timeline
    unit = Delta(T.REC_UNIT_DWORDS, T.REC_UNIT_BITS, T.REC_UNIT_DELTA_HEAD)
    proj = Delta(T.REC_PROJ_DWORDS, T.REC_PROJ_BITS, T.REC_PROJ_DELTA_HEAD)
    glob = None
    live = set()
    tick = None
    per_tick = []                   # (tick, live, unit deltas)
    deltas_now = 0
    lists = {}
    pend = None
    orders = collections.Counter()
    cells = collections.Counter()
    bracket = 0
    timeline = collections.defaultdict(collections.Counter)
    for kind, first, last, payload in bl:
        if kind != T.RB_STREAM:
            continue
        for rk, b in T.records(payload):
            n = 4 + ((len(b) + 3) & ~3)
            if rk == T.RK_TICK:
                if tick is not None:
                    per_tick.append((tick, len(live), deltas_now))
                tick = struct.unpack_from("<I", b)[0]
                deltas_now = 0
            if tick is not None:
                timeline[tick // window][GROUP_OF.get(rk, "other")] += n
            if rk == T.RK_UNIT:
                slot = struct.unpack_from("<H", b)[0]
                live.add(slot)
                unit.full(slot, b[4:4 + T.REC_UNIT_IMAGE])
            elif rk == T.RK_UNIT_DELTA:
                slot = struct.unpack_from("<H", b)[0]
                deltas_now += 1
                unit.delta(slot, b[2:2 + T.REC_UNIT_BITS], b[T.REC_UNIT_DELTA_HEAD:])
            elif rk == T.RK_UNIT_GONE:
                live.discard(struct.unpack_from("<H", b)[0])
            elif rk == T.RK_PROJ:
                proj.full(struct.unpack_from("<H", b)[0], b[4:])
            elif rk == T.RK_PROJ_DELTA:
                proj.delta(struct.unpack_from("<H", b)[0], b[2:2 + T.REC_PROJ_BITS],
                           b[T.REC_PROJ_DELTA_HEAD:])
            elif rk == T.RK_GLOBALS:
                glob = Delta(len(b) // 4, (len(b) // 4 + 7) // 8, 0)
                glob.full(0, b)
            elif rk == T.RK_GLOBALS_DELTA and glob is not None:
                nb = glob.nbits
                glob.delta(0, b[:nb], b[(nb + 3) & ~3:])
            elif rk == T.RK_ORDERS:
                slot, lst, total, first_ = struct.unpack_from("<HBxHH", b)
                key = (slot, lst)
                if first_ == 0:
                    pend = (key, bytearray(), total)
                    orders["lists sent"] += 1
                pend[1].extend(b[8:])
                orders["bytes"] += n
                if first_ + (len(b) - 8) // T.REC_ORDER_IMAGE >= total:
                    new = bytes(pend[1])
                    old = lists.get(key)
                    if old is not None and old == new:
                        orders["re-sent unchanged"] += 1
                    elif old is not None and len(old) == len(new):
                        orders["same length, changed"] += 1
                        orders["images in them"] += total
                        orders["images changed"] += sum(
                            old[i:i + T.REC_ORDER_IMAGE] != new[i:i + T.REC_ORDER_IMAGE]
                            for i in range(0, len(new), T.REC_ORDER_IMAGE))
                    else:
                        orders["new or resized"] += 1
                    lists[key] = new
                    pend = None
            elif rk in (T.RK_EV_FEATURE, T.RK_EV_DEATH):
                bracket += 1
            elif rk in (T.RK_EV_FEATURE_END, T.RK_EV_DEATH_END):
                bracket = max(0, bracket - 1)
            elif rk == T.RK_CELLS:
                x, z, cn, _ = struct.unpack_from("<HHHH", b)
                where = "inside a feature call or death (its input)" if bracket else "images"
                cells[where + ": records"] += 1
                cells[where + ": cells"] += cn
                cells[where + ": bytes"] += n
    if tick is not None:
        per_tick.append((tick, len(live), deltas_now))

    def field_table(d, label_of, top=25):
        rows = []
        for i, c in d.changed.most_common(top):
            hist = d.bits.get(i, {})
            tot = sum(hist.values())
            q = []
            if tot:
                acc = 0
                for bits in sorted(hist):
                    acc += hist[bits]
                    for pct in (50, 90, 99):
                        if acc * 100 >= pct * tot and f"p{pct}" not in dict(q):
                            q.append((f"p{pct}", bits))
            rows.append({"dword": i, "name": label_of(i), "changes": c, "bytes": 4 * c,
                         "bits": dict(q)})
        return rows

    gnames = T.known_names(h)
    out["unit_deltas"] = {
        "records": unit.records, "value_bytes": 4 * sum(unit.changed.values()),
        "overhead_bytes": unit.overhead,
        "fields": field_table(unit, lambda i: names[i])}
    out["projectile_deltas"] = {
        "records": proj.records, "value_bytes": 4 * sum(proj.changed.values()),
        "overhead_bytes": proj.overhead,
        "fields": field_table(proj, lambda i: f"+0x{4 * i:02X}", 12)}
    if glob is not None:
        out["globals_deltas"] = {
            "records": glob.records, "value_bytes": 4 * sum(glob.changed.values()),
            "fields": field_table(glob, lambda i: (gnames[i] if gnames and i < len(gnames)
                                                   else f"dword {i}"), 12)}
    out["orders"] = dict(orders)
    out["cells"] = dict(cells)
    if per_tick:
        busy = [d / l for _, l, d in per_tick if l]
        out["units_per_iteration"] = {
            "iterations": len(per_tick),
            "live_max": max(l for _, l, _ in per_tick),
            "live_last": per_tick[-1][1],
            "changed_fraction_mean": sum(busy) / len(busy) if busy else 0,
            "unit_deltas_mean": sum(d for _, _, d in per_tick) / len(per_tick)}
        out["live_curve"] = [(t, l) for t, l, _ in per_tick[::max(1, len(per_tick) // 16)]]
    out["timeline"] = {f"{w * window}-{(w + 1) * window - 1}": dict(c)
                       for w, c in sorted(timeline.items())}

    # other compressors on the same blocks, and on the stream as one piece
    streams = [p for k, _, _, p in bl if k == T.RB_STREAM]
    alt = {"writer (lodepng, 2 KB window)": report["stream"]["real_packed"],
           "zlib 9, 32 KB window": sum(len(zlib.compress(p, 9)) for p in streams),
           "lzma preset 6": sum(len(lzma.compress(p, preset=6)) for p in streams)}
    if zstandard:
        for lvl in (3, 19):
            c = zstandard.ZstdCompressor(level=lvl)
            alt[f"zstd {lvl}"] = sum(len(c.compress(p)) for p in streams)
        c = zstandard.ZstdCompressor(level=19, write_content_size=False)
        alt["zstd 19, the whole stream as one frame"] = len(c.compress(b"".join(streams)))
    out["compressors"] = alt
    return out


def kb(n):
    return f"{n / 1024:,.0f} KB" if n < 10 << 20 else f"{n / 1048576:,.1f} MB"


def show(r):
    print(f"{r['file']}: {kb(r['bytes'])}, {r['map']}, {r['players']} players, {r['slots']} slots")
    ups = r.get("units_per_iteration", {})
    if ups:
        print(f"  {ups['iterations']} iterations; live units at most {ups['live_max']}, "
              f"{ups['live_last']} at the end; on average {ups['unit_deltas_mean']:.0f} unit deltas "
              f"an iteration, {100 * ups['changed_fraction_mean']:.0f} % of the live units")
        print("  live units: " + ", ".join(f"{t}:{l}" for t, l in r["live_curve"]))
    print("\nthe file by block kind (on disk = packed + 32-byte block headers)")
    for k, v in sorted(r["blocks"].items(), key=lambda kv: -kv[1]["on_disk"]):
        print(f"  {k:<12} {v['n']:>6} blocks  raw {kb(v['raw']):>10}  on disk {kb(v['on_disk']):>10}"
              f"  {100 * v['on_disk'] / r['bytes']:5.1f} %")
    for label in ("stream", "snapshots"):
        s = r[label]
        if not s["real_packed"]:
            continue
        tot_raw = sum(g["raw"] for g in s["groups"].values())
        print(f"\nthe {label} by category: raw {kb(tot_raw)}, packed {kb(s['real_packed'])} "
              f"(recompressed alike {kb(s['recompressed'])}; categories deflated alone sum to "
              f"{kb(s['sum_alone'])})")
        print(f"  {'category':<28}{'records':>10}{'raw':>12}{'raw %':>8}{'packed alone':>14}{'%':>7}")
        for g, v in s["groups"].items():
            print(f"  {g:<28}{v['records']:>10}{kb(v['raw']):>12}{100 * v['raw'] / tot_raw:7.1f}%"
                  f"{kb(v['packed_alone']):>14}{100 * v['packed_alone'] / s['sum_alone']:6.1f}%")
    for key, title in (("unit_deltas", "unit deltas"), ("projectile_deltas", "projectile deltas"),
                       ("globals_deltas", "globals deltas")):
        d = r.get(key)
        if not d or not d["records"]:
            continue
        print(f"\n{title}: {d['records']} records, {kb(d['value_bytes'])} of changed values"
              + (f" + {kb(d['overhead_bytes'])} of headers and bitmaps" if "overhead_bytes" in d
                 else "") + "; changed dwords, most first (bits = the change's size, p50/p90/p99)")
        for f in d["fields"]:
            b = " ".join(f"{k} {v}" for k, v in f["bits"].items())
            print(f"  {f['name']:<44}{f['changes']:>10}  {kb(f['bytes']):>9}  {b}")
    if r.get("orders"):
        print("\norder lists: " + ", ".join(f"{k} {v:,}" for k, v in r["orders"].items()))
    if r.get("cells"):
        print("feature grid cells: " + ", ".join(f"{k} {v:,}" for k, v in r["cells"].items()))
    print("\nthe stream's raw bytes per window of GameTime (the five largest categories)")
    top = [g for g, _ in sorted(r["stream"]["groups"].items(),
                                key=lambda kv: -kv[1]["raw"])[:5]]
    print(f"  {'GameTime':<14}" + "".join(f"{g[:18]:>20}" for g in top) + f"{'all':>12}")
    for w, c in r["timeline"].items():
        print(f"  {w:<14}" + "".join(f"{kb(c.get(g, 0)):>20}" for g in top)
              + f"{kb(sum(c.values())):>12}")
    print("\nthe stream's blocks under other compressors")
    base = r["compressors"]["writer (lodepng, 2 KB window)"]
    for k, v in r["compressors"].items():
        print(f"  {k:<42}{kb(v):>12}  {v / base:6.2f}x the writer's")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("file")
    ap.add_argument("--json", help="also write the numbers to this file")
    ap.add_argument("--window", type=int, default=900, help="GameTime per timeline row (900 = 30 s)")
    a = ap.parse_args()
    r = analyse(a.file, a.window)
    show(r)
    if a.json:
        Path(a.json).write_text(json.dumps(r, indent=1))


if __name__ == "__main__":
    main()
