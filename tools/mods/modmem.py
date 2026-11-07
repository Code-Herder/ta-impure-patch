#!/usr/bin/env python3
"""The exe's large-address bit through the Mods screen on Wine: Confirm sets it, keeps the old file and restarts; a file check's revert is set again; a folder
that will not take the new file refuses with nothing changed; a bit-off entry puts the old file back.

    python3 tools/mods/modmem.py [--display :N] [--film FILE]

The verdicts read the exe on disk, its key, impure.cfg, the log and a skirmish played under the mod."""
import argparse
import json
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tools/mods"), str(ROOT / "tools/modpack")]
import modpack as mp  # noqa: E402
import modsscreen as ms  # noqa: E402
import runlock  # noqa: E402

MOD, NAME = "escalation", "Escalation"
report, tacli = ms.report, ms.tacli


def fields(d):
    """(the bit, the stored checksum, the checksum these bytes have)."""
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    at, total = pe + 88, 0
    for i in range(0, len(d), 2):
        total += 0 if at <= i < at + 4 else d[i] | (d[i + 1] << 8 if i + 1 < len(d) else 0)
        total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return bool(d[pe + 22] & 0x20), struct.unpack_from("<I", d, at)[0], (total + len(d)) & 0xFFFFFFFF


def key(d):
    """A known exe's key, which ignores this bit and the checksum: the same rule as tagpu_modpak_file_key."""
    return mp.file_key(d, True)


def confirm_page(g):
    tacli("ui", ms.NAME, "click", "MODS")
    return g.settled(lambda s: s["open"] and s["drawn"])


def relaunch(g):
    tacli("stop", ms.NAME, check=False)
    g.launch()


def main():
    a = argparse.ArgumentParser()
    a.add_argument("--display", required=True)
    a.add_argument("--film")
    args = a.parse_args()
    runlock.hold(ROOT, "modmem")
    film = None
    try:
        g = ms.Game(args.display)
        exe = next(p for p in g.dir.iterdir() if p.name.lower() == "totala.exe")
        kept, tmp, old = (exe.with_name(exe.name + s) for s in (".pre-4gb", ".4gb.tmp", ".4gb-old"))
        original = exe.read_bytes()
        exe.unlink()                # an instance's exe may be shared with others: this test's is its own
        exe.write_bytes(original)
        report(not fields(original)[0], "the folder's exe starts with the bit off")
        ms.install_by_hand(g, MOD)
        version = next(e["version"] for e in json.loads((ROOT / "mods/catalog.json").read_text())["mods"] if e["id"] == MOD)
        (g.dir / "mods" / MOD / "installed.txt").write_text(version)    # the marker an install writes
        if args.film:
            film = subprocess.Popen(["ffmpeg", "-y", "-v", "error", "-f", "x11grab", "-framerate", "15", "-video_size", "1280x720", "-i", args.display,
                                     "-c:v", "libx264", "-pix_fmt", "yuv420p", args.film], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        g.launch()

        # Confirm on an exe without the bit
        s = confirm_page(g)
        s = g.settled(lambda s: s["scan_done"] and ms.row_of(s, NAME) >= 0, 60)
        report(ms.row_of(s, NAME) >= 0, f"Manage lists the mod: {[r['name'] for r in s['rows']]}")
        g.click(f"act:{ms.row_of(s, NAME)}")
        s = g.settled(lambda s: s["confirm"])
        g.shot("mem-chosen")
        g.click_ending("confirm")
        report(g.menu(180), "the game comes back at the main menu after Confirm")
        now = exe.read_bytes()
        bit, stored, real = fields(now)
        report(bit, "Confirm set the large-address bit")
        report(stored == real and stored != 0, f"the header's checksum fits the file: stored {stored:#x}, computed {real:#x}")
        report(kept.read_bytes() == original, "the old file is kept, byte for byte, as .pre-4gb")
        report(key(now) == key(original), "the exe's key does not see the change")
        changed = [i for i in range(len(now)) if now[i] != original[i]]
        pe = struct.unpack_from("<I", original, 0x3C)[0]
        report(all(i == pe + 22 or pe + 88 <= i < pe + 92 for i in changed), f"only the bit and the checksum differ: {len(changed)} bytes")
        report(not tmp.exists(), "no working file is left beside it")
        report(f"mod={MOD}" in (g.dir / "impure.cfg").read_bytes().decode(errors="replace"), "impure.cfg names the mod")
        log = g.logs()
        report("modmem: the exe's large-address bit is on" in log, "the log says so")
        report(f"modload: {MOD}" in log, "the mod is running")
        tops = re.findall(r"modmem: this process's address space ends at (0x[0-9a-f]+)", log)
        report(tops and int(tops[-1], 16) > 0x80000000, f"the process that came back has the room: {tops[-1:]}")

        # the mod in a game, with the bit: the address space it needs, used
        for gadget in ("SINGLE", "Skirmish", "Start"):
            for _ in range(30):
                if tacli("ui", ms.NAME, "click", gadget, timeout=60, check=False).returncode == 0:
                    break
                time.sleep(2)
        entered = tacli("wait", ms.NAME, "alive=[1-9]", "--timeout", "180", timeout=200, check=False).returncode == 0
        time.sleep(6)
        g.shot("mem-game")
        report(entered, "a skirmish under the mod starts")
        tacli("stop", ms.NAME, check=False)

        # a file check puts the old exe back: the next Confirm sets it again
        exe.unlink()
        exe.write_bytes(original)
        g.launch()
        s = confirm_page(g)
        report(s["confirm"] == "Restart to apply", f"with the old exe back the screen offers a restart: {s['confirm']!r}")
        report("the next Confirm settles it" in g.logs(), "the log names the difference")

        # a folder that refuses the new file: nothing changes, and the screen says so
        tmp.mkdir()
        g.click("confirm")
        s = g.settled(lambda s: "could not be replaced" in s["note"], 10)
        g.shot("mem-refused")
        report("could not be replaced" in s["note"], f"Confirm refuses and says why: {s['note']!r}")
        report(exe.read_bytes() == original, "the exe is as it was")
        tmp.rmdir()
        g.click_ending("confirm")
        report(g.menu(180), "with the folder free the game comes back")
        report(fields(exe.read_bytes())[0], "the next Confirm set the bit again")
        report(kept.read_bytes() == original, "and the old file is still the one kept")

        # an entry marked bit-off: Confirm puts the kept file back
        catalog = json.loads((ROOT / "mods/catalog.json").read_text())
        next(e for e in catalog["mods"] if e["id"] == MOD)["memory"] = "bit-off"
        report(mp.check_catalog(catalog) == [], "the test catalog passes the schema")
        (g.dir / "tagpu_modcatalog.on").write_text(json.dumps(catalog))
        relaunch(g)
        s = confirm_page(g)
        report(s["confirm"] == "Restart to apply", f"a bit-off mod with the bit on offers a restart: {s['confirm']!r}")
        g.click_ending("confirm")
        report(g.menu(180), "the game comes back")
        report(exe.read_bytes() == original, "a bit-off entry's Confirm puts the old exe back, byte for byte")
        report(not kept.exists(), "the kept file went back, none left beside it")
        report(not old.exists() or len(old.read_bytes()) == len(original), "the file set aside is at most waiting for the next start")
        relaunch(g)
        report(not old.exists(), "the next start removes the file set aside")

        # the same with no copy to put back: the bit is cleared in place
        d = bytearray(original)
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        d[pe + 22] |= 0x20
        tacli("stop", ms.NAME, check=False)
        exe.unlink()
        exe.write_bytes(d)
        g.launch()
        s = confirm_page(g)
        g.click_ending("confirm")
        report(g.menu(180), "the game comes back with no copy to restore")
        now = exe.read_bytes()
        report(not fields(now)[0] and key(now) == key(original), "with no copy kept the bit is cleared and the key holds")
    finally:
        tacli("stop", ms.NAME, check=False)
        if film:
            film.terminate()
            film.wait(timeout=20)
    print(f"modmem: {ms.fails} failed")
    sys.exit(1 if ms.fails else 0)


if __name__ == "__main__":
    main()
