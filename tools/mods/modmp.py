#!/usr/bin/env python3
"""The multiplayer fingerprint in real network games on Wine, two instances each: who may start with whom.

    python3 tools/mods/modmp.py [--only dropin|gameplay|cosmetic|stock|older|olderhost] [--old-dll PATH] [--film FILE]

Each case's verdict is the game's own: START's grayed state read from the running battle room, a click on it placed by the
display server, and both sides reaching play or staying in the room. --old-dll is an older Impure build's ddraw.dll: v0.5's sends the
key and no print, v0.6.1's the print and no play revision. `stock` and `older` check that the host refuses such a joiner and says so,
`olderhost` that this build does not get into the older build's room. --film records the gameplay case's host from its start."""
import argparse
import json
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tools/compat"), str(ROOT / "tools/modpack")]
import dpport  # noqa: E402
import modpack as mp  # noqa: E402
import runlock  # noqa: E402
import tacompat as t  # noqa: E402
import talog  # noqa: E402

OUT = ROOT / "tmp" / "modmp"
DLL = ROOT / "tagpu/ddraw/ddraw.dll"
PORT = None                     # the cases' DirectPlay port, leased in main (tools/dpport.py)
LEASE = None                    # that lease, held to the end of the process
FILM = None
fails = 0


def report(ok, what):
    global fails
    fails += not ok
    print(f"{'ok  ' if ok else 'FAIL'}  {what}", flush=True)


def setup(name):
    return next(s for s in t.SETUPS if s["name"] == name)


def mutator(work, mid, name, path, data):
    """A mutator of ProTA replacing `path`, its package and its catalog entry. The folder stays out of the repository."""
    folder = work / mid
    (folder / path).parent.mkdir(parents=True, exist_ok=True)
    (folder / path).write_bytes(data)
    out = work / f"{mid}.json"
    subprocess.run([sys.executable, str(ROOT / "tools/modpack/modpack.py"), "mutator", str(folder), "--id", mid, "--base", "prota", "--name", name,
                    "--version", "1.0", "--author", "Tests", "--summary", "A test.", "--homepage", "https://example.invalid", "--permission", "the tests'",
                    "--out", str(out)], check=True, capture_output=True)
    return json.loads(out.read_text())


def add_mutator(inst, entry):
    """Into one instance: the package as an install leaves it, the switch on, and a catalog that names it."""
    gd = inst["gamedir"]
    mid = entry["id"]
    with tempfile.TemporaryDirectory() as d:
        subprocess.run(["zstd", "-dqf", str(mp.OUT / mid / f"{mid}.tar.zst"), "-o", d + "/p.tar"], check=True)
        with tarfile.open(d + "/p.tar") as tar:
            tar.extractall(gd / "mods" / mid, filter="data")
    (gd / "mods" / mid / "installed.txt").write_text(entry["version"])
    catalog = json.loads((ROOT / "mods/catalog.json").read_text())
    catalog["mods"].append(entry)
    (gd / "tagpu_modcatalog.on").write_text(json.dumps(catalog))
    (gd / "profiles").mkdir(exist_ok=True)
    with open(gd / "profiles" / "prota.ini", "ab") as f:
        f.write(f"mutator.{mid}=on\r\n".encode())


def log_of(inst):
    p = inst["gamedir"] / "log" / "tagpu.log"
    return p.read_text(errors="replace") if p.exists() else ""


def prints(inst):
    return re.findall(r"modprint: \d+ files of \d+, \d+ MB, \d+ ms: exe (\w+) content (\w+), (.*)", log_of(inst))


def screen(inst):
    return (t.tacli("ui", inst["name"], timeout=15).stdout.splitlines() or [""])[0]


class Pair:
    """Two prepared instances, started, at their main menus, on a port of their own; everything is stopped on the way out."""

    def __init__(self, label, host_setup, join_setup, host_dll=DLL, join_dll=DLL):
        self.label = label
        xv = t.start_xvfb()
        try:
            self.host = t.prepare_wine({**host_setup, "name": f"mp-{label}"}, host_dll, xv.tacompat_display)
            self.join = t.prepare_wine({**join_setup, "name": f"mp-{label}"}, join_dll, xv.tacompat_display, suffix="-j")
        finally:
            t.stop_xvfb(xv)
        self.games = {}

    def __enter__(self):
        for inst in (self.host, self.join):
            dpport.set_port(inst["prefix"], PORT)
        t.free_dplay_port(PORT, {str(self.host["prefix"]), str(self.join["prefix"])})
        # each peer's refusals of the GPU memory cap, read once it has stopped (talog.CapWatch)
        self.caps = {r: talog.CapWatch(inst["gamedir"]) for r, inst in (("host", self.host), ("join", self.join))}
        self.games["host"] = t.start_wine(self.host, None)
        self.film = None
        if FILM and self.label == "gameplay":
            self.film = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-video_size", "1280x1024", "-framerate", "15", "-f", "x11grab", "-i",
                                          f":{self.games['host']['display']}", "-vf", "crop=640:480:0:0", "-c:v", "libx264", "-preset", "veryfast",
                                          "-crf", "22", "-pix_fmt", "yuv420p", str(FILM)], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(3)
        self.games["join"] = t.start_wine(self.join, None)
        end = time.time() + t.POLL_S
        for inst in (self.host, self.join):
            while "MAINMENU" not in screen(inst):
                if time.time() > end:
                    raise RuntimeError(f"{inst['name']}: no main menu: {screen(inst)!r}")
                time.sleep(2)
        return self

    def __exit__(self, *exc):
        if self.film:
            self.film.communicate(b"q", timeout=30)
        for role, g in self.games.items():
            self.shot(role, "end")
            t.stop_wine(g)
        for role in self.games:
            refused = self.caps[role].failure(f"the {role}")
            if refused:
                report(False, f"{self.label}: {refused}")
        OUT.mkdir(parents=True, exist_ok=True)
        for role, inst in (("host", self.host), ("join", self.join)):
            (OUT / f"{self.label}-{role}.log").write_text(log_of(inst))
        return False

    def shot(self, role, what):
        OUT.mkdir(parents=True, exist_ok=True)
        subprocess.run(["import", "-display", f":{self.games[role]['display']}", "-window", "root", str(OUT / f"{self.label}-{role}-{what}.png")],
                       capture_output=True, timeout=20)

    def click_start_by_hand(self):
        """A press and release at START's rectangle, from the display server: nothing of tacli's refusal of a grayed button in the way."""
        shown = t.lobby_ui(self.host["name"], "show", "START")
        x, y, w, h = map(int, re.search(r"^rect\s+\[(-?\d+), (-?\d+), (\d+), (\d+)\]", shown, re.M).groups())
        x, y = x + w // 2, y + h // 2
        subprocess.run(["xdotool", "mousemove", str(x), str(y), "click", "1"], env={"DISPLAY": f":{self.games['host']['display']}", "PATH": "/usr/bin:/bin"},
                       capture_output=True, timeout=20)


def start_and_play(p):
    t.wine_lobby(p.host["name"], p.join["name"], PORT)
    return t.tacli("wait", p.join["name"], "alive=[1-9]", "--timeout", "20", timeout=70).returncode == 0


def dropin(row, equals):
    """A managed player and a drop-in player of one mod start, and hold one print."""
    with Pair(f"dropin-{row}", setup(f"managed-{row}"), setup(equals)) as p:
        report(start_and_play(p), f"{row}: the managed host and the drop-in joiner start")
        a, b = prints(p.host), prints(p.join)
        report(a and b and a[-1][:2] == b[-1][:2], f"{row}: one print on both sides: {a[-1:]} {b[-1:]}")
        report("modgate:" not in log_of(p.host) + log_of(p.join), f"{row}: nobody was named as differing")


def gameplay(work):
    """A mutator that changes a unit, on the host alone: START stays off on the host, the click does nothing, both are told."""
    base = mp.base_answer(mp.base_sources("prota"), "objects3d/armcom.3do", {})
    entry = mutator(work, "tmpgame", "Gameplay test", "objects3d/armcom.3do", base + b"\0")
    p = Pair("gameplay", setup("managed-prota"), setup("managed-prota"))
    add_mutator(p.host, entry)
    with p:
        t.lobby_to_ready(p.host["name"], p.join["name"], PORT)
        time.sleep(5)
        shown = t.lobby_ui(p.host["name"], "show", "START")
        report(re.search(r"^grayed\s+1\s*$", shown, re.M), "START is grayed on the host with both ready")
        for role, inst in (("host", p.host), ("join", p.join)):
            lines = re.findall(r"modgate: (.*)", log_of(inst))
            report(lines and "Gameplay test" in lines[0], f"the {role} was told: {lines[:1]}")
            report(len(lines) == 1, f"the {role} was told once, in {len(lines)} lines")
            p.shot(role, "blocked")
        p.click_start_by_hand()
        time.sleep(12)
        for inst in (p.host, p.join):
            report("LOUNGE2" in screen(inst), f"{inst['name']} stayed in the battle room after the click: {screen(inst)[:40]}")
        a, b = prints(p.host), prints(p.join)
        report(a and b and a[-1][1] != b[-1][1] and a[-1][0] == b[-1][0], f"the exe agrees and the content does not: {a[-1:]} {b[-1:]}")


def cosmetic(work):
    """A mutator that changes only a picture, on the host alone: they start, each with what they chose."""
    base = mp.base_answer(mp.base_sources("prota"), "anims/fx.gaf", {})
    entry = mutator(work, "tmplook", "Cosmetic test", "anims/fx.gaf", base + b"\0")
    p = Pair("cosmetic", setup("managed-prota"), setup("managed-prota"))
    add_mutator(p.host, entry)
    with p:
        report(start_and_play(p), "a host with a picture changed and a joiner without start")
        a, b = prints(p.host), prints(p.join)
        report(a and b and a[-1][:2] == b[-1][:2], f"one print on both sides: {a[-1:]} {b[-1:]}")
        report("Cosmetic test" in a[-1][2] if a else False, f"the host's label names it: {a[-1:]}")


def refused(p, why):
    """The joiner is sent back and the host's battle room says so, once; the host stays in its room."""
    joined = True
    try:
        t.lobby_to_ready(p.host["name"], p.join["name"], PORT)
    except (t.Lobby, SystemExit) as e:
        joined = False
        print(f"      the walk ended: {str(e)[:160]}", flush=True)
    # an older joiner can reach the room for a moment before the host's refusal lands: wait for the line and for the joiner to leave
    end = time.time() + 60
    while time.time() < end and not ("modgate: said:" in log_of(p.host) and "LOUNGE2" not in screen(p.join)):
        time.sleep(2)
    time.sleep(3)
    p.shot("host", "refused")
    p.shot("join", "refused")
    said = re.findall(r"modgate: said: (.*)", log_of(p.host))
    report("LOUNGE2" not in screen(p.join), f"the joiner is not in the room (the walk {'finished' if joined else 'ended'}): {screen(p.join)[:40]}")
    report(len(said) == 1 and said[0].endswith("refused: " + why), f"the host was told once, {why}: {said}")
    report("LOUNGE2" in screen(p.host), f"the host stayed in its room: {screen(p.host)[:40]}")


def stock(_work):
    """A joiner whose data is stock's 21 bytes: refused, and the host told it is not running Impure."""
    p = Pair("stock", setup("managed-prota"), setup("managed-prota"))
    (p.join["gamedir"] / "tagpu_modprint_stock.on").write_text("")
    with p:
        refused(p, "not running Impure")


def older(old):
    """An older Impure joining, with no print or with no play revision: refused, and the host told it is an older build."""
    with Pair("older", setup("managed-prota"), setup("managed-prota"), join_dll=old) as p:
        refused(p, "older Impure build")


def olderhost(old):
    """This build joining an older host, which refuses it by its size test or by the engine's own version test: the joiner is not in the room."""
    with Pair("olderhost", setup("managed-prota"), setup("managed-prota"), host_dll=old) as p:
        joined = True
        try:
            t.lobby_to_ready(p.host["name"], p.join["name"], PORT)
        except (t.Lobby, SystemExit) as e:
            joined = False
            print(f"      the walk ended: {str(e)[:160]}", flush=True)
        # the refusal can land a moment after the joiner's room comes up
        end = time.time() + 60
        while time.time() < end and "LOUNGE2" in screen(p.join):
            time.sleep(2)
        time.sleep(3)
        p.shot("host", "refused")
        p.shot("join", "refused")
        report("LOUNGE2" not in screen(p.join), f"the joiner is not in the room (the walk {'finished' if joined else 'ended'}): {screen(p.join)[:40]}")
        report("LOUNGE2" in screen(p.host), f"the host stayed in its room: {screen(p.host)[:40]}")


def main():
    a = argparse.ArgumentParser(description=__doc__)
    a.add_argument("--only", choices=["dropin", "gameplay", "cosmetic", "stock", "older", "olderhost"])
    a.add_argument("--old-dll", help="an older Impure build's ddraw.dll (v0.5's: no fingerprint; v0.6.1's: no play revision); `older` and `olderhost` need it")
    a.add_argument("--film", type=Path)
    args = a.parse_args()
    global FILM, PORT, LEASE
    FILM = args.film
    # each case a host and a joiner at once; their instances carry the run's tag (tacompat.inst_name),
    # and OUT is one folder for every run
    runlock.hold(ROOT, "modmp", 2, [f"shots:{OUT}"], suite="compat")
    why = runlock.together(2, "modmp")          # the host and the joiner meet in one lobby
    if why:
        print(f"could not run: {why}", file=sys.stderr)
        return 2
    LEASE = dpport.lease()
    PORT = LEASE.port
    OUT.mkdir(parents=True, exist_ok=True)
    old = OUT / "older-ddraw.dll"
    if args.only in (None, "older", "olderhost"):
        if not args.old_dll:
            a.error("--old-dll is needed for the older and olderhost cases")
        shutil.copy2(args.old_dll, old)
    with tempfile.TemporaryDirectory() as d:
        work = Path(d)
        if args.only in (None, "dropin"):
            dropin("prota", "prota-4.8")
            dropin("tazero", "tazero-alpha5")
        if args.only in (None, "gameplay"):
            gameplay(work)
        if args.only in (None, "cosmetic"):
            cosmetic(work)
        if args.only in (None, "stock"):
            stock(work)
        if args.only in (None, "older"):
            older(old)
        if args.only in (None, "olderhost"):
            olderhost(old)
    print(f"modmp: {fails} failed", flush=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
