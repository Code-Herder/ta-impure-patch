#!/usr/bin/env python3
"""fxpools_check -- a process's second level starts with empty effect pools, in one launch that plays two
skirmishes (tagpu/ddraw/src/tagpu_patches.c, THE EFFECT POOLS START EVERY LEVEL EMPTY).

    tools/fxpools_check.py <instance> [--scenario NAME] [--keep-dll] [--gpu]

Level one is the scenario (default scenarios/fx-mix.json: two lines of units that fight at once), played until its
explosion pool holds a written record at or past its count and a debris record has carried a shatter's body. That
reading is the control: it is what stock's level reset leaves in place, since it zeroes the explosion count alone
(0x420630). The level is then left for the main menu through the exit menu (Tab, EXIT, MAINMENU, y), which runs the
teardown 0x491B60, and a skirmish is started from the main menu (SINGLE, Skirmish, Start) with the settings the
scenario left. At the second level's first in-play packet one peek reads, at one instant (tagpu_peek_frame evaluates a
trigger's every line in one call on the game thread):
  - the explosion pool's count and its first RECS records: every record at or past the count is zero;
  - the first DEBRIS debris records with their vertex and face blocks: each free one (+0 = 0xFF) holds exactly what
    the level load writes there (debris_init), and its vertices are zero;
  - the first 100 flying-piece slots: zero.
These are the bytes a fresh process holds at a level's start. The second level starts with commanders alone, so a
record below the count is the level's own and is not judged.

The pools are the build's: stock's in the main block and .data, the raised build's at the addresses its `limits:
installed` line logs (a debris record's +0x24 and +0x28 name its blocks in either build). Exit 0 when the second
level's pools are empty, 1 when they are not, 2 when the check could not run (level one never filled the pools, a menu
did not answer).

The launch is one game lane of the run lock with the instance's resource (resource_regression's hold_instance). It
renders with GDI (tagpu_gdi.on in the game folder, put back as the run found it) and takes no GPU token; --gpu renders
it with the default renderer. --keep-dll launches the ddraw.dll already in the instance, which is how the stock-limits
build is run.
"""
import argparse
import json
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

TREE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TREE / "tools"))
from resource_regression import TACLI, cap_failure, hold_instance, tacli_api  # noqa: E402

LOAD_S = 900            # a launch to a live map, a cold prefix included
FILL_S = 240            # level one's fight filling the pools, in wall-clock seconds
MENU_S = 120            # a menu step, and the second level's load
LOOK_S = 2.0            # between two readings of level one's pools
RECS = 64               # explosion records judged
DEBRIS = 16             # debris records judged, with their blocks
PIECES = 100            # flying-piece slots judged (stock's whole array)
START_TICKS = 600       # the second level's reading is of its start: GameTime restarts at 0 every level
EXPL_REC, AUX_REC, AUX_VERTS, AUX_FACES = 0x54, 0x34, 0x60, 0xC0
MAIN_PP = 0x511DE8
GAMETIME = "*0x511DE8+0x38a47"
# THE DEBRIS RECORD AS THE LEVEL LOAD LEAVES IT (0x4207FB..0x4208F9, debris_init): +0 0xFF free, +4 8 vertices,
# +8 6 faces, +0xC -1, +0x1C 0x502C28, +0x20 0, +0x24 its vertex block, +0x28 its face block; each face +0 0xC8,
# +4 4, +0xC 0x502BF8 + 8k, +0x1C 1. A fresh process's every other byte there is zero.
FACE_IDX, REC_CONST = 0x502BF8, 0x502C28
RAISED = re.compile(r"limits: installed \d+ sites.*? explosions (\d+) at 0x([0-9A-Fa-f]{8}), flying pieces (\d+) at "
                    r"0x([0-9A-Fa-f]{8}), debris records (\d+) at 0x([0-9A-Fa-f]{8})")
STOCK = re.compile(r"limits: stock build")
FIRST_PACKET = re.compile(r"packet: level gen \d+: first in-play packet|units: alive=[1-9]")


class Game:
    def __init__(self, api, instance):
        self.api, self.name = api, instance
        self.inst = api.Instance(instance)

    def call(self, command, *args, timeout=150):
        r = subprocess.run([sys.executable, str(TACLI), command, self.name, *map(str, args)],
                           capture_output=True, text=True, timeout=timeout)
        if r.returncode:
            raise OSError(f"tacli {command} {' '.join(map(str, args))}: {(r.stdout + r.stderr).strip()[-400:]}")
        return json.loads(r.stdout) if "--json" in args else r.stdout

    def peek(self, specs):
        rows = self.call("peek", *specs, "--json")["peeks"]
        if [r["spec"] for r in rows] != list(specs):
            raise OSError(f"peek answered {len(rows)} of {len(specs)} specs")
        return [r["value"] for r in rows]

    def gui(self):
        return self.call("ui", "--json").get("gui")

    def until(self, test, what, seconds):
        end = time.monotonic() + seconds
        while not (got := test()):
            if time.monotonic() > end:
                raise OSError(f"{what} within {seconds} s")
            time.sleep(0.5)
        return got


def region(addr, length):
    """The specs that read [addr, addr + length), 256 bytes a spec at most (tagpu_peek.h)."""
    out, off = [], 0
    while off < length:
        n = min(0x100, length - off)
        out.append(f"0x{addr + off:08X}:x{n:X}")
        off += n
    return out


class Pools:
    """Where the build keeps the three pools, and one reading of them."""

    def __init__(self, g):
        text = g.api.talog.run_text(g.inst.gamedir)
        m = RAISED.search(text)
        main = int(g.peek([f"0x{MAIN_PP:08X}"])[0].split()[0])
        if m:
            self.build = "raised"
            self.expl, self.pieces, self.debris = int(m.group(2), 16), int(m.group(4), 16), int(m.group(6), 16)
        elif STOCK.search(text):
            self.build = "stock"
            self.expl, self.pieces, self.debris = main + 0x1491B, 0x511DF0, main + 0x1AB9F
        else:
            raise OSError("the log names no limits line: which pools the build uses is unknown")
        rec0 = bytes.fromhex(g.peek(region(self.debris, AUX_REC))[0])
        self.verts, self.faces = struct.unpack_from("<II", rec0, 0x24)
        self.layout = [("expl", self.expl, 4 + RECS * EXPL_REC), ("debris", self.debris, DEBRIS * AUX_REC),
                       ("verts", self.verts, DEBRIS * AUX_VERTS), ("faces", self.faces, DEBRIS * AUX_FACES),
                       ("pieces", self.pieces, PIECES * 4)]

    def read(self, g):
        specs = [GAMETIME]
        spans = []
        for name, addr, length in self.layout:
            s = region(addr, length)
            spans.append((name, len(s)))
            specs += s
        vals = g.peek(specs)
        out, k = {"tick": int(vals[0].split()[0])}, 1
        for name, n in spans:
            if any(v == "<unreadable>" for v in vals[k:k + n]):
                raise OSError(f"the {name} pool is unreadable")
            out[name] = b"".join(bytes.fromhex(v) for v in vals[k:k + n])
            k += n
        out["count"] = struct.unpack_from("<i", out["expl"], 0)[0]
        return out

    def record(self, r, k):
        return r["expl"][4 + k * EXPL_REC:4 + (k + 1) * EXPL_REC]

    def tail(self, r):
        """The written records at or past the count, among the first RECS."""
        lo = max(r["count"], 0)
        return [k for k in range(lo, RECS) if any(self.record(r, k))]

    def used_debris(self, r):
        """The debris records whose vertex block is written: each has carried a shatter's body."""
        return [i for i in range(DEBRIS) if any(r["verts"][i * AUX_VERTS:(i + 1) * AUX_VERTS])]

    def debris_wrong(self, r):
        """The free debris records among the first DEBRIS that differ from the level load's, with what differs."""
        bad = []
        for i in range(DEBRIS):
            rec = r["debris"][i * AUX_REC:(i + 1) * AUX_REC]
            if rec[0] != 0xFF:
                continue
            want = bytearray(AUX_REC)
            want[0] = 0xFF
            struct.pack_into("<IIi", want, 4, 8, 6, -1)
            struct.pack_into("<IIII", want, 0x1C, REC_CONST, 0, self.verts + i * AUX_VERTS,
                             self.faces + i * AUX_FACES)
            face = bytearray(AUX_FACES)
            for f in range(6):
                struct.pack_into("<II", face, f * 0x20, 0xC8, 4)
                struct.pack_into("<I", face, f * 0x20 + 0xC, FACE_IDX + 8 * f)
                struct.pack_into("<I", face, f * 0x20 + 0x1C, 1)
            what = []
            if bytes(want) != rec:
                what.append("record")
            if any(r["verts"][i * AUX_VERTS:(i + 1) * AUX_VERTS]):
                what.append("vertices")
            if bytes(face) != r["faces"][i * AUX_FACES:(i + 1) * AUX_FACES]:
                what.append("faces")
            if what:
                bad.append(f"{i} ({', '.join(what)})")
        return bad


def run(api, instance, scenario, keep_dll, gpu, out):
    g = Game(api, instance)
    gamedir = g.inst.gamedir
    gdi = gamedir / "tagpu_gdi.on"
    had_gdi = gdi.exists()                  # the lever is its presence alone (tagpu_cfg.c)
    fails = []
    try:
        gdi.unlink(missing_ok=True)         # unlinked first: a link is never written through
        if not gpu:
            gdi.write_text("")
        r = subprocess.run([sys.executable, str(TACLI), "scenario", "load", instance, str(scenario), "--defaults",
                            "--restart", *(["--keep-dll"] if keep_dll else [])],
                           capture_output=True, text=True, timeout=LOAD_S)
        if r.returncode:
            raise OSError(f"scenario load: {(r.stdout + r.stderr).strip()[-400:]}")
        pools = Pools(g)
        out.append(f"  build {pools.build}: explosions at 0x{pools.expl:08X}, debris at 0x{pools.debris:08X} "
                   f"(vertices 0x{pools.verts:08X}, faces 0x{pools.faces:08X}), pieces at 0x{pools.pieces:08X}")

        # LEVEL ONE, THE CONTROL: records at or past the count, and a debris record that carried a body
        end = time.monotonic() + FILL_S
        while True:
            one = pools.read(g)
            tail, used = pools.tail(one), pools.used_debris(one)
            if tail and used:
                break
            if time.monotonic() > end:
                raise OSError(f"level one did not fill the pools in {FILL_S} s (GameTime {one['tick']}: count "
                              f"{one['count']}, {len(tail)} written record(s) past it, {len(used)} used debris "
                              "record(s))")
            time.sleep(LOOK_S)
        out.append(f"  level one at GameTime {one['tick']}: explosion count {one['count']}, {len(tail)} written "
                   f"record(s) at or past it (first {tail[0]}), debris records {used} carried a body")

        # out to the main menu: the exit menu's MAINMENU runs the teardown 0x491B60
        g.call("keys", "tab")
        g.call("ui", "click", "EXIT")
        g.call("ui", "click", "MAINMENU")
        g.call("keys", "y")
        g.until(lambda: g.gui() == "MAINMENU.GUI", "the main menu did not come back", MENU_S)

        # LEVEL TWO, from the menu, in the same process
        lines = api._LogLines(api._log_cursor(g.inst))
        for button in ("SINGLE", "Skirmish", "Start"):
            g.call("ui", "click", button)
        g.until(lambda: any(FIRST_PACKET.search(ln) for ln in lines()), "the second level did not start", MENU_S)
        two = pools.read(g)
        tail = pools.tail(two)
        wrong = pools.debris_wrong(two)
        pieces = [k for k in range(PIECES) if any(two["pieces"][4 * k:4 * k + 4])]
        out.append(f"  level two at GameTime {two['tick']}: explosion count {two['count']}, {len(tail)} written "
                   f"record(s) at or past it; free debris records unlike the load's: {len(wrong)}; flying-piece "
                   f"slots written: {len(pieces)}")
        if two["tick"] > START_TICKS:
            raise OSError(f"the reading at GameTime {two['tick']} is not of a level's start")
        if tail:
            fails.append(f"explosion records {tail} at or past the count {two['count']} hold an earlier level's bytes")
        if wrong:
            fails.append(f"free debris records {wrong} differ from the level load's")
        if pieces:
            fails.append(f"flying-piece slots {pieces} are written at the level's start")
    finally:
        try:
            subprocess.run([sys.executable, str(TACLI), "stop", instance], capture_output=True, text=True,
                           timeout=300)
        finally:
            gdi.unlink(missing_ok=True)
            if had_gdi:
                gdi.write_text("")
    return fails


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("instance")
    p.add_argument("--scenario", default="fx-mix.json",
                   help="a scenario in scenarios/ whose fight fills the effect pools (default fx-mix.json)")
    p.add_argument("--keep-dll", action="store_true", help="launch the ddraw.dll already in the instance")
    p.add_argument("--gpu", action="store_true", help="render with the default renderer and take a GPU token")
    a = p.parse_args()
    api = tacli_api()
    hold_instance(f"fxpools_check on {a.instance}", a.instance, gpu=a.gpu)
    out = []
    try:
        fails = run(api, a.instance, TREE / "scenarios" / a.scenario, a.keep_dll, a.gpu, out)
    except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError, ValueError) as e:
        print("\n".join(out))
        print(f"fxpools_check: COULD NOT RUN: {e}")
        return 2
    refused = cap_failure(a.instance)
    if refused:
        fails.append(refused)
    print("\n".join(out))
    for f in fails:
        print(f"  FAIL {f}")
    print(f"fxpools_check: {'FAIL' if fails else 'PASS'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
