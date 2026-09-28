#!/usr/bin/env python3
"""tacompat -- does a ddraw.dll start, refuse or crash beside TADR, the Community Patch
Loader and the mods' own executables?

A *setup* (setups.json) is a game folder as a player has it: the retail 3.1 game, what a
mod or patch install puts next to it (fixtures.json), and the ddraw.dll under test.

    tacompat.py fetch                                  # download every fixture that can be
    tacompat.py fetch --import NAME=PATH               # a fixture behind a browser check
    tacompat.py list                                   # setups, fixtures, what is missing
    tacompat.py wine    [--dll PATH] [SETUP ...]       # every setup at once, on Wine
    tacompat.py wine    --battle 0 --mp 0              # start-up only: no skirmish, no network game
    tacompat.py windows [--dll PATH] [SETUP ...]       # one at a time, on a Windows desktop
    tacompat.py clean                                  # remove the Wine instances
    tacompat.py selftest                               # check the hook decode, no game needed

`wine` gives each setup its own tacli instance (a private registry, see prepare_wine) and
its own virtual display, and runs them in parallel. `windows` copies the player's folder
once, then rebuilds one work folder per setup and starts the game on the machine's
desktop through a scheduled task. Both watch every window the game opens for the whole
run and read what each party logs. On Wine, a setup that reaches the main menu then
starts a skirmish and fights 200 against 200, because two patchers that both start can
still collide where the limits are used: `battle-crash` is a crash after the menu. Where
Impure runs, a second instance of the same folder then joins it in a two-player network game
over Windows' DirectPlay, several at once, each on a DirectPlay port of its own. Every run, and
every peer, is read for TADR's code having run two ways: from the game folder (tadr_evidence)
and from the running process (exe_hooks) -- the exe's own code against TotalA.exe on disk, which
is the only thing that shows a recorder started off the exe's entry point.
Each setup has a `goal` (Impure active, no TADR code run) and, until the takeover reaches
it, `today`: the behaviour accepted meanwhile. A run is "meets goal", "known gap" (matches today) or
UNEXPECTED. Exit status: 1 on anything UNEXPECTED (with --strict, on a known gap too),
0 otherwise, 2 when nothing could run.

The guide to the routes and outcomes: the wiki's Compatibility section, research/notes/compat/.
"""

import argparse
import concurrent.futures
import hashlib
import json
import ntpath
import os
import queue
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(TREE / "tools"))
import taremote  # noqa: E402  PowerShell over SSH, one checked statement a line
import dpport  # noqa: E402  DirectPlay's port, per prefix (tools/dpport.py)

TACLI = TREE / "tools" / "tacli"
DPINSTALL = TREE / "tools" / "dpinstall.sh"
DPLAY_SRC = Path(os.environ.get("TA_DIRECTPLAY_SRC", Path.home() / ".local/share/ta-directplay"))
# NO NAT HELPER. dpwsockx loads dpnhupnp and dpnhpast when they load (DirectPlayNATHelpCreate)
# and skips them when they do not (0x5DF03C90..0x5DF03CB2, DISASSEMBLED); with one in, every
# game process opens a UDP socket on each of the machine's interfaces for UPnP discovery,
# asks the LAN's router to map its ports, and tests every candidate data port against the
# helper's answer (0x5DF088B1). The suite's games meet on loopback and need none of it -- and a
# test has no business mapping ports on the router of whoever runs it. dplaysvr.exe, which
# references them too, is started by the game and inherits this environment.
DPLAY_OVERRIDES = "dplayx,dpmodemx,dpnet,dpwsockx,dplaysvr.exe,dpnsvr.exe=n;dpnhpast,dpnhupnp=d"
# NO SOUND, AND A SOUND DEVICE: nothing the suite checks listens, and a dozen games at once
# through the owner's speakers is noise -- but a game with no device at all is a different game
# (TA:ESC stops at "No sound driver is available for use."). So Wine is kept off PulseAudio and
# its ALSA driver opens ALSA's default device, which asound-null.conf makes the null plugin: the
# game plays, the samples go nowhere, and no file of the setup changes (a mod's own totala.ini is
# part of the setup under test, so NoDirectSound is not written into it).
AUDIO_OFF = "winepulse.drv=d"
ASOUND_NULL = HERE / "asound-null.conf"
RETAIL_MD5 = "8e74a1dffa1f5988624c52048f5b20cd"      # TotalA.exe 3.1, pristine/manifest.md5
PREFIX = "compat-"                                   # every Wine instance this tool owns


def main_checkout() -> Path:
    """tacli's instances live in the main checkout (tacli's repo_root), whichever tree
    this tool runs from."""
    if os.environ.get("TACLI_ROOT"):
        return Path(os.environ["TACLI_ROOT"]).expanduser().resolve()
    try:
        common = subprocess.run(["git", "-C", str(TREE), "rev-parse", "--path-format=absolute",
                                 "--git-common-dir"], capture_output=True, text=True,
                                check=True).stdout.strip()
        return Path(common).parent
    except (subprocess.CalledProcessError, OSError):
        return TREE


INSTANCES = main_checkout() / "tagpu" / "instances"
# OUTSIDE the repository, beside the DirectPlay files: third-party binaries, and results
# whose logs carry absolute paths, must never be one `git add -A` away from a commit.
CACHE = Path(os.environ.get("TACOMPAT_CACHE", Path.home() / ".local/share/ta-compat"))
FIXTURES = CACHE / "fixtures"
DOWNLOADS = CACHE / "downloads"
RESULTS = CACHE / "results"
WINDOWS_CFG = CACHE / "windows.json"     # the machine's address and the player's folder

# The boxes that end a run, by title. Each party refuses through a MessageBox and
# ExitProcess(0xC1) (ERROR_BAD_EXE_FORMAT): Impure (tagpu_patches.c), TADR
# (EngineLimits::AbortIfInstallFailed), the Patch Loader (its DllMain, exit(1)).
KNOWN_BOXES = {
    "Total Annihilation: Impure cannot start": "impure-refused",
    "Total Annihilation: Impure cannot load these units": "impure-refused",
    "TADR engine-limit error": "tadr-refused",
    "Total Annihilation Community Patch": "loader-refused",
}
CRASH_TITLES = re.compile(r"program error|serious problem|stopped working|cessé de fonctionner"
                          r"|has encountered|werfault", re.I)
OUTCOMES = ("impure-active", "impure-inactive", "impure-not-loaded", "impure-refused",
            "tadr-refused", "loader-refused", "crash", "battle-crash", "other-box", "exited",
            "no-result")
REFUSED_EXIT = 0xC1
BATTLE_MAP = "Two Continents"          # scenarios/200v200.json's setup.map
# Whether any of TADR's code ran, from what each part leaves in the GAME FOLDER. tdraw.dll
# writes tdrawlog.txt from its DllMain, and says there when it has written its engine patches
# (the old limit crack, the 2026 EngineLimits). The recorder -- tplayx.dll, or the 2006
# dplayx.dll -- creates "log\TA Demo Recorder Log -<date>.txt" only on the path that starts
# it from inside a DirectPlay export, so such a log written during the run IS the evidence;
# a "DLL.DirectPlay..." line in it names the export.
#
# THIS HALF CAN NEVER PROVE THE NEGATIVE: a recorder started from the jump its DllMain splices
# over the exe's entry point writes no log at all (research/notes/compat/takeover.md, part 1).
# That is what exe_hooks is for, and a goal of tadr_ran: false is judged on both.
TADR_INSTALLED = re.compile(r"Install Limit Crack|\[EngineLimits\] installed")
# The whole of what TADR's DllMain has done on the routes where it is what loads Impure: it
# writes this line before LoadLibrary("ddraw.dll") returns, so the game folder cannot come up
# empty there however completely the takeover stops the rest (setups.json: tadr_started).
TADR_ONLY_STARTED = re.compile(r"(?:.*: )?tdraw started \(tdrawlog\.txt\)$")
# ...and what that file may say for it to count as "only started": the one line TADR writes
# before it loads Impure. Read from the FILE, not from the summary above it -- a build whose
# DllMain ran on and logged something else produces the same summary string.
TADR_START_LINE = re.compile(r"^\s*\d+\s+---\s+Process Attached\.\s+config=\S+\s*$")
RECORDER_LOG = re.compile(r"Demo Recorder Log", re.I)
RECORDER_CALLED = re.compile(r"^\s*DLL\.DirectPlay", re.M)
# The two-player stage: small halves applied one per peer, each as that peer's own units.
MP_SCENARIOS = ("compat-mp-host", "compat-mp-join")
# DirectPlay's name server binds its port for the whole machine, so every network game of a
# run gets a port of its own from these, patched into both peers' prefixes (tools/dpport.py).
MP_PORT_BASE = dpport.STOCK + 1
# An archive the game reads and never writes is linked into a Wine folder, not copied.
LINKED = {".ufo", ".gp3", ".hpi", ".ccx", ".ufo2"}


def die(msg, code=2):
    print(f"tacompat: {msg}", file=sys.stderr)
    sys.exit(code)


def md5_file(path: Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_json(name):
    return json.loads((HERE / name).read_text())


FIX = load_json("fixtures.json")["fixtures"]
SETUPS = load_json("setups.json")["setups"]


# ------------------------------------------------------------------------- fixtures

def fixture_problems(name) -> list:
    """What is missing or wrong in the cached copy of a fixture (empty: ready)."""
    spec = FIX[name]
    out = []
    for rel, want in spec["files"].items():
        p = FIXTURES / name / rel
        if not p.is_file():
            out.append(f"{rel} missing")
        elif md5_file(p) != want:
            out.append(f"{rel} md5 differs")
    return out


def _install_members(name, read):
    """Write a fixture's files from `read(rel) -> bytes`, checking each against the
    manifest, into a temporary folder that replaces the cached one only when all pass."""
    spec = FIX[name]
    dest = FIXTURES / name
    tmp = Path(tempfile.mkdtemp(prefix=f".{name}.", dir=FIXTURES))
    try:
        for rel, want in spec["files"].items():
            data = read(rel)
            got = hashlib.md5(data).hexdigest()
            if got != want:
                raise SystemExit(f"tacompat: {name}: {rel} has md5 {got}, the manifest pins {want}")
            (tmp / rel).parent.mkdir(parents=True, exist_ok=True)
            (tmp / rel).write_bytes(data)
        if dest.exists():
            shutil.rmtree(dest)
        tmp.rename(dest)
    finally:
        if tmp.exists():
            shutil.rmtree(tmp)


def _from_path(name, src: Path):
    spec = FIX[name]
    strip = spec.get("strip", "")
    if src.is_dir():
        def read(rel):
            for cand in (src / rel, src / strip / rel):
                if cand.is_file():
                    return cand.read_bytes()
            raise SystemExit(f"tacompat: {name}: {rel} is not in {src}")
    elif zipfile.is_zipfile(src):
        z = zipfile.ZipFile(src)
        names = {n.lower(): n for n in z.namelist()}

        def read(rel):
            key = names.get((strip + rel).lower()) or names.get(rel.lower())
            if key is None:
                raise SystemExit(f"tacompat: {name}: {rel} is not in {src.name}")
            return z.read(key)
    else:
        if len(spec["files"]) != 1:
            raise SystemExit(f"tacompat: {name}: {src} is neither a folder nor a zip")
        data = src.read_bytes()

        def read(rel):
            return data
    _install_members(name, read)


def cmd_fetch(args):
    FIXTURES.mkdir(parents=True, exist_ok=True)
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    imports = {}
    for item in args.imports or []:
        name, _, path = item.partition("=")
        if name not in FIX or not path:
            die(f"--import {item!r}: say NAME=PATH with NAME one of {', '.join(FIX)}")
        imports[name] = Path(path).expanduser()
    bad = 0
    for name, spec in FIX.items():
        if name in imports:
            _from_path(name, imports[name])
            print(f"{name}: imported from {imports[name]}")
            continue
        if not fixture_problems(name):
            print(f"{name}: ready")
            continue
        if "url" not in spec:
            print(f"{name}: MISSING -- fetch by hand: {spec['manual']}\n"
                  f"    then: tacompat.py fetch --import {name}=<the file or folder>")
            bad += 1
            continue
        target = DOWNLOADS / (spec.get("archive") or Path(spec["url"]).name)
        if not target.is_file() or md5_file(target) != spec["md5"]:
            print(f"{name}: downloading {spec['url']}")
            req = urllib.request.Request(spec["url"], headers={"User-Agent": "tacompat"})
            with urllib.request.urlopen(req, timeout=120) as r, open(target, "wb") as f:
                shutil.copyfileobj(r, f)
        got = md5_file(target)
        if got != spec["md5"]:
            print(f"{name}: the download has md5 {got}, the manifest pins {spec['md5']} -- "
                  f"the file changed upstream; nothing installed")
            bad += 1
            continue
        _from_path(name, target)
        print(f"{name}: ready")
    return 1 if bad else 0


def cmd_list(args):
    print("fixtures:")
    for name, spec in FIX.items():
        p = fixture_problems(name)
        print(f"  {name:24} {'ready' if not p else 'NOT READY: ' + '; '.join(p)}")
    print("setups:")
    for s in SETUPS:
        need = [a["fixture"] for a in s["add"] if fixture_problems(a["fixture"])]
        state = "ready" if not need else "needs " + ", ".join(sorted(set(need)))
        today = s.get("today", {}).get("outcome")
        print(f"  {s['name']:22} {'today ' + today if today else 'meets the goal':24} {state}")
        print(f"  {'':22} {s['about']}")
    return 0


def overlay(setup) -> list:
    """[(name in the game folder, cached file)] in the order the setup adds them; a
    later entry replaces an earlier one of the same name, as a later install would."""
    out = {}
    for a in setup["add"]:
        name, files = a["fixture"], a["files"]
        if files == "*":
            pairs = [(rel, rel) for rel in FIX[name]["files"]]
        elif isinstance(files, dict):
            pairs = list(files.items())
        else:
            pairs = [(f, f) for f in files]
        for dest, src in pairs:
            if src not in FIX[name]["files"]:
                die(f"setup {setup['name']}: {src} is not a file of fixture {name}")
            out[dest.lower()] = (dest, FIXTURES / name / Path(src))
    return list(out.values())


def pick_setups(names) -> list:
    if not names:
        return list(SETUPS)
    by = {s["name"]: s for s in SETUPS}
    missing = [n for n in names if n not in by]
    if missing:
        die(f"no setup {', '.join(missing)} (tacompat.py list)")
    return [by[n] for n in names]


def expectation(setup, platform, which) -> "dict | None":
    """The setup's `goal` or `today` for this platform ('today_windows' overrides fields
    of 'today' there); None when the setup has no such entry."""
    if which not in setup:
        return None
    exp = dict(setup[which])
    exp.update(setup.get(f"{which}_{platform}", {}))
    return exp


def verdict(o, setup, platform):
    """Meets the goal; or matches today's accepted behaviour (a known gap); or neither."""
    goal = expectation(setup, platform, "goal")
    o["goal"], o["differs"] = goal, judge(o, goal)
    if not o["differs"]:
        o["verdict"] = "meets goal"
        return
    today = expectation(setup, platform, "today")
    o["today"] = today
    if today is not None and not judge(o, today):
        o["verdict"] = "known gap"
    else:
        o["verdict"] = "UNEXPECTED"
        if today is not None:
            o["differs_today"] = judge(o, today)


# ------------------------------------------------------------------------- outcome

# THE UI LAYER'S HEALTH, read from the consumer's own lines (tagpu_vk_gui.c `behind_ex`). A
# fresh start the consumer asks for is it finding its store out of step with the producer's; the
# first few frames of a session legitimately ask (the store has seeded nothing yet), and nothing
# else should. Two reasons were one-frame disagreements answered with a fresh start that caused
# the next one, until the consumer gave up and composited nothing -- a black screen that no other
# check here sees, because every other check reads state and gadgets, not pixels (MEASURED
# 2026-09-27: every Escalation run gave up, Total Mayhem's skirmish screen went black on Windows).
GUI_ASK = re.compile(r"^vk: gui: the twin store cannot follow the producer \(([^)]*)\)", re.M)
GUI_GAVE_UP = "fresh starts have not made the twin store able to follow the producer"
GUI_STARTUP = {"an op names a surface this store never seeded", "the presented surface has no twin here"}
GUI_STRESS = re.compile(r"^gui: reseedstress: fresh start (\d+) asked by the harness", re.M)


def lever_file(lever) -> tuple:
    """A lever is a file put in the game folder: a name alone for an empty file (an `.off`
    switch), or {"file": name, "text": contents} for one whose contents are the setting."""
    if isinstance(lever, dict):
        return lever["file"], lever.get("text", "")
    return lever, ""


GUI_INDEXED = re.compile(r"^vk: gui: a restored sprite arrived while this lane holds no restored atlas", re.M)
# the producer's repaint budget for one palette generation, spent: what is on screen keeps its indices
GUI_SPENT = "gui: the restored UI atlas has settled 32 times in one palette generation"
# WHY A UI BOX CROSSED AS ENGINE PIXELS, which the consumer drops (its `pixdrop=`): the producer's
# cumulative count by cause, every 600 frames. One cause is a bug and never expected: `copy-freed`,
# a copy whose source surface was dropped before the copy was published -- the stale orders panel
# of v0.3. The others are the UI layer's known work list (a draw kind with no op of its own, a
# source never drawn through an observed leaf) and are reported, not judged.
GUI_PIXELS = re.compile(r"^GUI pixels: (.*)$", re.M)
GUI_PIXELS_BUG = {"copy-freed"}


def gui_pixels(text: str) -> dict:
    """The last `GUI pixels:` line as {cause: count}; empty when none was written."""
    lines = GUI_PIXELS.findall(text)
    if not lines or lines[-1].strip() == "none":
        return {}
    words = lines[-1].split()
    return {k: int(v) for k, v in zip(words[::2], words[1::2]) if v.isdigit()}


def gui_health(text: str) -> dict:
    """The consumer's own fresh-start requests by reason -- split at the stress lever's first
    fresh start, since what the session asks before it is the ordinary start-up -- whether it
    gave up, how many fresh starts the `reseedstress=` lever asked for, and how often a restored
    sprite was drawn indexed (the fallback the stress exists to exercise)."""
    first = GUI_STRESS.search(text)
    cut = first.start() if first else len(text)
    asks, asks_stress = {}, {}
    for m in GUI_ASK.finditer(text):
        into = asks if m.start() < cut else asks_stress
        into[m.group(1)] = into.get(m.group(1), 0) + 1
    fired = [int(m) for m in GUI_STRESS.findall(text)]
    return {"asks": asks, "asks_stress": asks_stress, "gave_up": GUI_GAVE_UP in text,
            "stress": max(fired, default=0), "indexed": len(GUI_INDEXED.findall(text)),
            "spent": GUI_SPENT in text, "pixels": gui_pixels(text)}


def gui_misses(g: "dict | None", exp: dict) -> list:
    """The UI layer's part of `judge`: never given up, never out of step past the start-up --
    and under the stress lever, not out of step at all, with the lever seen to fire."""
    if g is None or exp["outcome"] != "impure-active":
        return []
    miss = []
    if g["gave_up"]:
        miss.append("the UI layer gave up following the game and composited nothing (a black screen)")
    stress = exp.get("gui_stress")
    # Past the start-up nothing may ask; under the lever, nothing at all may, whatever the reason.
    bad = {r: n for r, n in g["asks"].items() if r not in GUI_STARTUP}
    for r, n in g.get("asks_stress", {}).items():
        bad[r] = bad.get(r, 0) + n
    if bad:
        miss.append("the UI layer asked for fresh starts of its own: " +
                    ", ".join(f"{n}x {r}" for r, n in sorted(bad.items())))
    if stress and g["stress"] < stress:
        miss.append(f"the stress lever asked for {g['stress']} fresh starts, fewer than {stress}")
    # A stress run that never met the race proves nothing: the fallback has to have run.
    if stress and not g["indexed"]:
        miss.append("no restored sprite ever met a blanked atlas, so the race was not exercised")
    # Every fresh start's indexed frame is answered by a repaint; with the budget spent the
    # indexed art stays, which is the dithered picture the fix exists to avoid.
    if stress and g.get("spent"):
        miss.append("the repaint budget ran out, so indexed art stayed on screen")
    bug = {k: n for k, n in g.get("pixels", {}).items() if k in GUI_PIXELS_BUG and n}
    if bug:
        miss.append("UI copies were dropped because their source was freed first: " +
                    ", ".join(f"{n}x {k}" for k, n in sorted(bug.items())))
    return miss


def packet_pub(text: str) -> int:
    """The largest frame-packet count the heartbeat reported: frames Impure drew."""
    return max((int(m) for m in re.findall(r"^packet:\S* pub=(\d+) skip=", text, re.M)), default=0)


def classify(o: dict) -> str:
    """One word for what happened. A crash outranks everything; then the first known
    refusal box; then an unknown box; then how the process ended; then whether Impure
    loaded and drew."""
    if o.get("watched") is False:
        return "no-result"          # the window watcher never finished: nothing seen is evidence
    titles = [b["title"] for b in o["boxes"]]
    if o.get("errorlog") or any(CRASH_TITLES.search(t) for t in titles):
        return "battle-crash" if o.get("menu") and o.get("battle") is not None else "crash"
    for t in titles:
        if t in KNOWN_BOXES:
            return KNOWN_BOXES[t]
    if titles:
        return "other-box"
    if not o["alive_at_end"]:
        if o.get("exit_code") == REFUSED_EXIT and o.get("failure"):
            return "impure-refused"
        return "exited"
    if not o["impure_loaded"]:
        return "impure-not-loaded"
    return "impure-active" if o["packet_pub"] > 0 else "impure-inactive"


def _started_only(tdrawlog) -> bool:
    """Whether tdrawlog.txt holds nothing but the line TADR writes before it loads Impure.
    Anything else in it is TADR's code having run after that, which no allowance covers."""
    if not tdrawlog:
        return True                      # no file at all: nothing to allow
    lines = [ln for ln in tdrawlog.splitlines() if ln.strip()]
    return len(lines) == 1 and bool(TADR_START_LINE.match(lines[0]))


def judge(o: dict, exp: dict) -> list:
    """Every way the run differs from what the setup expects (empty: as expected)."""
    miss = []
    if o["outcome"] != exp["outcome"]:
        miss.append(f"outcome {o['outcome']}, expected {exp['outcome']}")
    if "tdrawlog" in exp and exp["tdrawlog"] not in (o.get("tdrawlog") or ""):
        miss.append(f"tdrawlog.txt does not say {exp['tdrawlog']!r}")
    if "failure" in exp and exp["failure"] not in (o.get("failure") or ""):
        miss.append(f"startup-failure.txt does not name {exp['failure']}")
    loaded = [Path(p.replace("\\", "/")).name.lower() for p in o.get("folder_modules", [])]
    if o.get("modules_known"):
        for dll in exp.get("loaded", []):
            if dll.lower() not in loaded:
                miss.append(f"{dll} was not loaded from the game folder")
        for dll in exp.get("not_loaded", []):
            if dll.lower() in loaded:
                miss.append(f"{dll} was loaded from the game folder")
    if exp["outcome"] == "impure-active" and o.get("menu") is False:
        miss.append("the main menu was never reached")
    ran = o.get("tadr_ran") or []
    if exp.get("tadr_started") and _started_only(o.get("tdrawlog")):
        # On the routes where TADR's DllMain is what loads Impure, it has written its first log
        # line before Impure exists. That one line is the whole of it -- no engine patch, no
        # recorder log, and nothing of its code after -- so it is allowed here and nothing else
        # is: every other piece of evidence still fails the setup, and so does a tdrawlog.txt
        # that holds anything beyond that line.
        ran = [e for e in ran if not TADR_ONLY_STARTED.match(e)]
    if "tadr_ran" in exp and bool(ran) != exp["tadr_ran"]:
        miss.append("TADR ran: " + "; ".join(ran) if ran else "TADR did not run")
    # Claiming no TADR code ran needs the reading of the process, not only the log files
    # (see hook_evidence): a comparison that could not be made leaves the claim unproven, on
    # the single-player run and on each peer of the network game alike.
    if exp.get("tadr_ran") is False:
        peers = [("", o.get("hooks"), True)]
        mp = o.get("mp")
        if mp is not None:
            peers += [(f"network game, {r}: ", (mp.get(r) or {}).get("hooks"), mp.get("ok", False))
                      for r in ("host", "join")]
        for who, h, expected in peers:
            # `h is None` is a read that never happened -- no process left to read, no `code`
            # event from the Windows watcher -- and that is not a pass either: the claim is
            # "nothing of TADR's ran", and nothing looked. A peer of a network game that never
            # started is not held to it.
            if expected and (h is None or h.get("why")):
                miss.append(who + hook_note(h))
    c = o.get("content")
    if c is not None and (c.get("why") or c.get("missing")):
        miss.append("the mod's own content is not loaded: " + (c.get("why") or
                    f"{', '.join(c['missing'])} missing from the engine's {c.get('types', 0)} unit types"))
    if o.get("battle") is not None and not o["battle"].get("ok", False) and exp["outcome"] != "battle-crash":
        miss.append(f"the battle failed: {o['battle'].get('why', '?')}")
    if o.get("mp") is not None and not o["mp"].get("ok", False):
        miss.append(f"the network game failed: {o['mp'].get('why', '?')}")
    miss += gui_misses(o.get("gui"), exp)
    miss += hud_misses((o.get("battle") or {}).get("hud"), exp)
    return miss


def tadr_evidence(tdrawlog, logs: dict, where="") -> list:
    """What says TADR's code ran: tdrawlog.txt at all, and a recorder log at all -- the
    recorder creates one only when it starts from a DirectPlay export. `logs` maps a log
    folder file's name to its text. What this cannot see: see RECORDER_LOG."""
    ev = []
    if tdrawlog is not None:
        ev.append(f"{where}tdraw started (tdrawlog.txt" +
                  (", engine patches installed)" if TADR_INSTALLED.search(tdrawlog) else ")"))
    for name, text in sorted(logs.items()):
        if RECORDER_LOG.search(name) or RECORDER_CALLED.search(text or ""):
            called = ", it answered a call" if RECORDER_CALLED.search(text or "") else ""
            ev.append(f"{where}the recorder ran (log\\{name}{called})")
    return ev


def other_logs(folder: Path, since: float) -> dict:
    """The game folder's log\\ files that are not Impure's, written since `since`."""
    out = {}
    if folder.is_dir():
        for f in folder.iterdir():
            if (f.is_file() and not f.name.lower().startswith("tagpu")
                    and f.name.lower() != "startup-failure.txt" and f.stat().st_mtime >= since):
                out[f.name] = f.read_text(errors="replace")
    return out


def snapshot_dll(dll: Path) -> Path:
    """The DLL under test, copied once, before anything runs.

    EVERY COPY A RUN MAKES COMES FROM THIS ONE FILE, and the report names its hash. A run is
    long and the DLL is rebuilt beside it: the network stage's joiner instance copies the DLL
    well after the start, and the report used to hash the path when it finished -- so a rebuild
    mid-run produced a run of two builds, reported as a third."""
    snap = Path(tempfile.mkdtemp(prefix="tacompat-dll-")) / "ddraw.dll"
    shutil.copy2(dll, snap)
    return snap


def report(results, platform, dll, started, strict=False, source=None) -> int:
    out = RESULTS / f"{time.strftime('%Y%m%d-%H%M%S', time.localtime(started))}-{platform}"
    out.mkdir(parents=True, exist_ok=True)
    for r in results:
        d = out / r["setup"]
        d.mkdir(exist_ok=True)
        for name, text in r.pop("_files", {}).items():
            (d / name).write_text(text, errors="replace")
        for name, src in r.pop("_copies", {}).items():
            if Path(src).exists():
                shutil.copy2(src, d / name)
    (out / "summary.json").write_text(json.dumps(
        {"platform": platform, "dll": str(source or dll), "dll_md5": md5_file(dll),
         "started": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(started)),
         "results": results}, indent=2) + "\n")
    n = {v: sum(r["verdict"] == v for r in results) for v in ("meets goal", "known gap", "UNEXPECTED")}
    lines = [f"tacompat {platform}: {n['meets goal']} meet the goal, {n['known gap']} known gaps, "
             f"{n['UNEXPECTED']} UNEXPECTED of {len(results)} "
             f"(dll md5 {md5_file(dll)[:12]}, {time.time() - started:.0f} s)"]
    mark = {"meets goal": "ok ", "known gap": "gap", "UNEXPECTED": "BAD"}
    for r in results:
        box = f" [{r['boxes'][0]['title']}]" if r["boxes"] else ""
        mp = r.get("mp")
        net = ("no mp" if r.get("mp_skip") else "-") if mp is None else \
              ("mp ok" if mp["ok"] else "mp FAILED")
        lines.append(f"  {mark[r['verdict']]} {r['setup']:22} {r['outcome']:18} {net:9}{box}")
        if r["verdict"] == "UNEXPECTED":
            for m in r.get("differs_today") or r.get("differs", []):
                lines.append(f"      {m}")
        if r.get("why"):                 # a run that threw: the exception is the row
            lines.append(f"      {r['why']}")
    lines.append(f"  results: {out}")
    text = "\n".join(lines)
    (out / "summary.txt").write_text(text + "\n")
    print(text)
    if n["UNEXPECTED"] or (strict and n["known gap"]):
        return 1
    return 0


# ------------------------------------------------------------------------- Wine

def tacli(*argv, timeout=120):
    return subprocess.run([sys.executable, str(TACLI), *argv], capture_output=True, text=True,
                          timeout=timeout)


DISPLAYS = threading.Lock()      # `taken` is reserved from the runs' threads as well


def free_display(taken: set) -> int:
    with DISPLAYS:
        for n in range(180, 400):
            if n in taken or Path(f"/tmp/.X11-unix/X{n}").exists() or Path(f"/tmp/.X{n}-lock").exists():
                continue
            taken.add(n)
            return n
    die("no free X display number between :180 and :399")


def start_xvfb(n: int) -> subprocess.Popen:
    p = subprocess.Popen(["Xvfb", f":{n}", "-screen", "0", "1280x1024x24", "-nolisten", "tcp"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    p.tacompat_display = n
    for _ in range(100):
        if Path(f"/tmp/.X11-unix/X{n}").exists():
            return p
        time.sleep(0.05)
    stop_xvfb(p)
    die(f"Xvfb :{n} did not start")


def stop_xvfb(p: subprocess.Popen):
    """Stop an Xvfb this runner started and take its display number back. SIGTERM, not
    SIGKILL: a killed Xvfb leaves /tmp/.X<n>-lock behind, free_display counts a lock as a
    display in use, and a few hundred runs exhaust :180 to :399. The lock is removed here
    too -- Xvfb does not always get to it -- and only for a display this runner owns."""
    n = getattr(p, "tacompat_display", None)
    p.terminate()
    try:
        p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    if n is not None:
        for stale in (Path(f"/tmp/.X{n}-lock"), Path(f"/tmp/.X11-unix/X{n}")):
            try:
                stale.unlink()
            except OSError:
                pass


def inst_name(setup_name: str, suffix="") -> str:
    """The tacli instance name for a setup. tacli takes 32 characters, and both instances of a
    setup have to fit inside them -- the single-player one and the joiner's `-j` -- so the room is
    always measured as though the suffix were there and a long name is cut to its head plus three
    hex digits of its hash, which keeps two long names of one family apart. `retail+tadr-recorder-
    ota` is the first that does not fit."""
    base = re.sub(r"[^a-z0-9]+", "-", setup_name.lower()).strip("-")
    room = 32 - len(PREFIX) - 2
    if len(base) > room:
        base = (base[:room - 4].rstrip("-") + "-" +
                hashlib.md5(base.encode()).hexdigest()[:3])
    return PREFIX + base + suffix


def prepare_wine(setup, dll: Path, display: int, suffix="") -> dict:
    """A fresh tacli instance holding the setup's folder.

    Its registry is made PRIVATE: `tacli create` clones the prefix with hardlinks, so
    every instance's user.reg is one inode, and TA, TADR and the Patch Loader all write
    the registry. So the hives are copied into new files, but only after the wineserver
    that `create` started (for Wine's own settings) has exited: it rewrites the shared
    hive when it goes (tacli-shared-registry-inode). Creates therefore run one at a time."""
    name = inst_name(setup["name"], suffix)
    tacli("rm", name, "--force")
    r = tacli("create", name, "--display", f":{display}", "--res", "1024x768")
    if r.returncode != 0:
        die(f"tacli create {name}: {(r.stderr or r.stdout).strip()}")
    try:
        meta = json.loads((INSTANCES / name / "instance.json").read_text())
    except (OSError, ValueError) as e:
        die(f"{name}: tacli made no readable instance.json ({e})")
    gamedir, prefix = Path(meta["gamedir"]), Path(meta["prefix"])
    env = dict(os.environ, WINEPREFIX=str(prefix))
    subprocess.run(["wineserver", "-w"], env=env, timeout=60)
    for hive in ("user.reg", "system.reg", "userdef.reg"):
        p = prefix / hive
        if p.exists():
            tmp = p.with_suffix(".compat-tmp")
            shutil.copy2(p, tmp)
            os.replace(tmp, p)
    if DPLAY_SRC.is_dir():
        subprocess.run(["bash", str(DPINSTALL), str(prefix), str(DPLAY_SRC)],
                       capture_output=True, check=True)
    # The skirmish map the battle is built for (scenarios/200v200.json). The SKIRMISH
    # screen loads the saved map's preview the moment it opens, so a map the setup lacks
    # is a box before any gadget can pick another. A mod whose loader moves the registry
    # (Mayhem: Software\TotalM) names its root in the setup. The hives are private by now.
    for root in ["Software\\Cavedog Entertainment"] + setup.get("registry_roots", []):
        subprocess.run(["wine", "reg", "add", f"HKCU\\{root}\\Total Annihilation", "/v", "SkirmishMap",
                        "/t", "REG_SZ", "/d", BATTLE_MAP, "/f"], env=env, capture_output=True, timeout=120)
    subprocess.run(["wineserver", "-w"], env=env, timeout=60)
    for dest, src in overlay(setup):
        target = gamedir / dest
        target.parent.mkdir(parents=True, exist_ok=True)
        for existing in target.parent.iterdir():
            if existing.name.lower() == target.name.lower() and (existing.is_symlink() or existing.is_file()):
                existing.unlink()
        if src.suffix.lower() in LINKED:
            target.symlink_to(src)
        else:
            shutil.copy2(src, target)
    # A launch here is a player's, not tacli's: the DLL takes a tacli-state folder
    # without tacli's command-line token for a stale launcher and refuses to run
    # (tagpu_regstore.h), so the instance's store is set aside.
    store = gamedir / taremote.STATE_DIR
    if store.is_dir():
        store.rename(gamedir / (taremote.STATE_DIR + ".off"))
    ddraw = gamedir / "ddraw.dll"
    ddraw.unlink(missing_ok=True)
    shutil.copy2(dll, ddraw)
    for leftover in ("ErrorLog.txt", "tdrawlog.txt"):
        (gamedir / leftover).unlink(missing_ok=True)
    for lever in setup.get("levers", []):
        name, text = lever_file(lever)
        (gamedir / name).write_bytes(text.encode())
    return {"name": name, "gamedir": gamedir, "prefix": prefix}


def x_windows(display: int) -> list:
    """(title, width, height) of every named top-level window on the display, less the
    input method's and DirectPlay's own helper (a 1x1 window a session opens)."""
    r = subprocess.run(["xwininfo", "-root", "-tree"], env=dict(os.environ, DISPLAY=f":{display}"),
                       capture_output=True, text=True, timeout=10)
    out = []
    for m in re.finditer(r'^\s+0x[0-9a-f]+ "(.*)": \("[^"]*" "[^"]*"\)\s+(\d+)x(\d+)', r.stdout, re.M):
        if m.group(1) not in ("Default IME", "DPlayHelpWndClass"):
            out.append((m.group(1), int(m.group(2)), int(m.group(3))))
    return out


def main_window(title, w, h) -> bool:
    """The game's own window: TA's shell is 640x480 at the least. TA's own error boxes
    are titled plain 'Total Annihilation' too, and are far smaller than that."""
    if not title.startswith("Total Annihilation") or title in KNOWN_BOXES:
        return False
    return title != "Total Annihilation" or (w >= 560 and h >= 400)


def new_boxes(display, seen, boxes, t0):
    for title, w, h in x_windows(display):
        if not main_window(title, w, h) and (title, w, h) not in seen:
            seen.add((title, w, h))
            boxes.append({"title": title, "size": f"{w}x{h}", "t": round(time.time() - t0, 1)})


def mod_content(setup, inst) -> "dict | None":
    """WHETHER THE MOD ITSELF IS RUNNING, not only Impure beside it: the unit types the setup
    names -- ones only that mod defines -- looked up in the engine's own table of loaded unit
    types (`tacli units`, which walks the definition table in memory, not any archive path). A
    loader that never started leaves stock TA with the mod's files lying beside it, and every
    other check passes that. None where the setup has no content of its own to show."""
    want = (setup.get("content") or {}).get("units") or []
    if not want:
        return None
    r = tacli("units", inst["name"], "--json", "--limit", "0", timeout=90)
    try:
        rows = json.loads(r.stdout)["units"]
    except (ValueError, KeyError, TypeError):
        return {"want": want, "why": f"tacli units: {(r.stderr or r.stdout).strip()[:200]}"}
    have = {str(row.get("name") if isinstance(row, dict) else row).upper() for row in rows}
    return {"want": want, "types": len(have), "missing": [u for u in want if u.upper() not in have]}


def alive_seen(gamedir: Path) -> "int | None":
    """The newest `units: alive=N` header in the game's tagpu.log: the units in the frame packet,
    which is what Impure draws and what `tacli roster` lists."""
    try:
        text = (gamedir / "log" / "tagpu.log").read_text(errors="replace")
    except OSError:
        return None
    found = re.findall(r"^units: alive=(\d+)", text, re.M)
    return int(found[-1]) if found else None


# THE SIDE PANEL AFTER A DESELECT, against the engine's own surface. Selecting a unit opens
# the orders panel over the side panel; deselecting restores what was under it by a copy out
# of a save-under the engine frees in the same call. The UI layer used to lose that copy and
# keep the panel on screen (MEASURED 2026-09-27: Escalation, 22.5% of the strip wrong on
# v0.3; 0.1% with the fix, which is colour rounding). Nothing else here reads pixels of the
# UI, so a stale region passes every other check.
HUD_STRIP = (0, 140, 128, 845)      # x, y, w, h: the side panel below the minimap, at 1280x1024
HUD_FUZZ = "9.4%"                   # a channel off by more than 24 of 255 is a different pixel
HUD_STALE = 0.05                    # past this, the screen is not showing the engine's panel
HUD_OPENED = 0.20                   # the selection must change the engine's own strip this much


def hud_strip_diff(a: Path, b: Path) -> "float | None":
    """The share of HUD_STRIP's pixels that differ between two captures, or None when
    either capture is missing or ImageMagick cannot compare them."""
    x, y, w, h = HUD_STRIP
    crop = f"[{w}x{h}+{x}+{y}]"
    if not (a.exists() and b.exists()):
        return None
    r = subprocess.run(["compare", "-metric", "AE", "-fuzz", HUD_FUZZ, f"{a}{crop}", f"{b}{crop}",
                        "null:"], capture_output=True, text=True, timeout=60)
    try:
        return float(r.stderr.split()[0]) / (w * h)
    except (ValueError, IndexError):
        return None


def hud_deselect(name, display, where: Path) -> dict:
    """Select the commander (ctrl+c), deselect it, and compare the side panel on screen with
    the engine's surface. `opened` is how much the deselect changed the engine's own strip --
    the proof the step exercised anything -- and `stale` the share of the strip where the
    screen disagrees with the engine after it.

    WHICH CLICK DESELECTS IS THE PLAYER'S SETTING. Under the default layout a right click on
    the ground deselects and a left click orders a move; under the right-click layout it is
    the other way round (MEASURED 2026-09-27: Escalation's instance deselects on the right
    click, retail's gives a move order). So the right click goes first, and when the engine's
    strip has not changed, a left click on other ground."""
    where.mkdir(parents=True, exist_ok=True)

    def capture(step):
        pair = (where / f"{step}-screen.png", where / f"{step}-engine.png")
        subprocess.run(["import", "-display", f":{display}", "-window", "root", str(pair[0])],
                       capture_output=True, timeout=30)
        tacli("shot", name, "-o", str(pair[1]), timeout=30)
        return pair

    # The left click aims elsewhere: under the right-click layout the right click was a move
    # order to its own point, and the commander may be standing on it three seconds later.
    try:
        tacli("keys", name, "ctrl+c", timeout=20)
        time.sleep(3)
        sel = capture("selected")
        for click in (["--right", name, "700", "600"], [name, "900", "400"]):
            tacli("click", *click, timeout=20)
            time.sleep(3)
            desel = capture("deselected")
            opened = hud_strip_diff(sel[1], desel[1])
            if opened is None or opened >= HUD_OPENED:
                break
        return {"opened": opened, "stale": hud_strip_diff(*desel), "where": str(where)}
    except subprocess.TimeoutExpired as e:
        return {"opened": None, "stale": None, "where": f"{where}: timed out: {e.cmd[:2]}"}


def hud_misses(hud: "dict | None", exp: dict) -> list:
    """The side-panel step's part of `judge`, for a setup where Impure draws."""
    if hud is None or exp["outcome"] != "impure-active":
        return []
    if hud.get("opened") is None or hud.get("stale") is None:
        return [f"the side panel could not be compared ({hud.get('where')})"]
    if hud["opened"] < HUD_OPENED:
        return [f"selecting the commander changed {hud['opened']:.0%} of the engine's side panel, "
                f"so the deselect check was not exercised"]
    if hud["stale"] > HUD_STALE:
        return [f"after a deselect the side panel on screen differs from the engine's at "
                f"{hud['stale']:.1%} of its pixels ({hud.get('where')})"]
    return []


def wine_battle(inst, proc, display, gamedir, seen, boxes, t0, seconds) -> dict:
    """From the main menu into a skirmish and a 200-a-side fight, then watch it: two
    patchers that both started can still collide in play, where the limits are used.
    The menus are driven by tacli's gadget layer and the units placed by its scenario
    applier (scenarios/200v200.json), as in any tacli run."""
    name = inst["name"]
    # A CLICK WAITS FOR ITS SCREEN. A mod with more content than stock takes longer between
    # these screens than a click's own patience allows, and Escalation goes from Skirmish
    # straight towards a load: both read as "no active gui" or a gadget that is not there yet,
    # neither of which is a failure. So each click is retried while that is what it says, and the
    # walk stops early if the game screen is already up (MEASURED 2026-09-27: Escalation's two
    # setups, whose battle stage no run had ever reached before the takeover let them start).
    in_game = False
    for gadget in ("SINGLE", "Skirmish", "Start"):
        if in_game or re.search(r"ARMMAIN|CORMAIN",
                                (tacli("ui", name, timeout=20).stdout.splitlines() or [""])[0]):
            break
        end, last = time.time() + 45, ""
        while True:
            r = tacli("ui", name, "click", gadget, timeout=60)
            if r.returncode == 0:
                break
            last = (r.stderr or r.stdout).strip()[:200]
            # The click landed on the game screen: this mod's front end needed fewer of them
            # than stock's (Escalation's Skirmish goes straight into a game, so there is no
            # Start to press), and the walk is done.
            if re.search(r"on (ARM|COR)MAIN", last):
                in_game = True
                break
            if time.time() >= end or not re.search(r"no active gui|is not on|no gadget", last):
                return {"ok": False, "why": f"ui click {gadget}: {last}"}
            time.sleep(3)
    deadline = time.time() + 90
    while time.time() < deadline:
        time.sleep(3)
        if proc.poll() is not None:
            return {"ok": False, "why": "the game exited while loading the skirmish"}
        first = (tacli("ui", name, timeout=20).stdout.splitlines() or [""])[0]
        if re.search(r"ARMMAIN|CORMAIN", first):
            break
    else:
        return {"ok": False, "why": "the skirmish never reached the game screen"}
    # Before the scenario: the start is the one moment the commander is sure to exist and the
    # view to hold nothing else a right click could land on.
    hud = hud_deselect(name, display, gamedir / "hudcheck")
    r = tacli("scenario", "apply", name, "200v200", timeout=180)
    if r.returncode != 0:
        return {"ok": False, "why": f"scenario apply: {(r.stderr or r.stdout).strip()[:200]}", "hud": hud}
    applied = (r.stdout.splitlines() or [""])[0]
    # THE UNITS ARE SEEN, not only created: the scenario applier counts what the engine made,
    # and a game whose units exist but that Impure cannot see -- no frame packet entry, so
    # nothing drawn and nothing in the roster -- passes every other check. The header is
    # written every half second; at least nine in ten of what was made must be in it.
    made = re.search(r"\((\d+) units?\b", applied)        # "applied 402 of 402 (401 units + 1 feature)"
    if not made:
        return {"ok": False, "hud": hud, "why": f"the applier's count is unreadable, so the units seen cannot be judged ({applied})"}
    want = int(made.group(1))
    time.sleep(4)
    got = alive_seen(gamedir)
    if got is None or got < max(1, want * 9 // 10):
        return {"ok": False, "hud": hud, "why": f"Impure sees {got if got is not None else 'no'} unit(s) of "
                                    f"the {want} the scenario made ({applied})"}
    end = time.time() + seconds
    while time.time() < end:
        time.sleep(2)
        new_boxes(display, seen, boxes, t0)
        if proc.poll() is not None:
            return {"ok": False, "hud": hud, "why": f"the game exited during the battle ({applied})"}
        if boxes or (gamedir / "ErrorLog.txt").exists():
            return {"ok": False, "hud": hud, "why": f"a box or a crash report during the battle ({applied})"}
    return {"ok": True, "why": applied, "hud": hud}


# ------------------------------------------------------- the running game's own code

# WHETHER ANY OF TADR'S CODE RAN, read from OUTSIDE the game: the exe's executable sections
# in the live process against TotalA.exe on disk, every run of changed bytes decoded for the
# address it leads to, and that address tested against the modules loaded from the game
# folder. A TADR module is one whose file carries TADR's own chat-channel name; a hook into
# any other DLL of the folder is a byte the mod itself sets (the Community Patch Loader
# rewrites three of the exe's import thunks into direct calls to the mod's win32.dll) and is
# reported, never counted as TADR.
#
# This is the evidence the log files cannot give: the recorder's way in through the exe's
# entry point writes nothing anywhere (research/notes/compat/takeover.md, part 1). The DLL
# makes the same comparison from inside and refuses the launch on a TADR finding
# (tagpu_takeover.h, pass 4); this is the independent check of that check, and the shapes
# decoded here are the same ones, so the two answer the same question.
TADR_MARK = b"TADemo-MKChat"
HOOK_CTX = 8               # bytes of a changed run's context kept each side, for the decode
CODE_FLAGS = 0x20000020    # IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE


def pe_sections(exe: bytes):
    """(the file's image base, [(name, va, vsize, raw, rsize, characteristics)]) of a 32-bit
    PE. `va` is already an address, not an RVA."""
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    n = struct.unpack_from("<H", exe, pe + 6)[0]
    opt = struct.unpack_from("<H", exe, pe + 20)[0]
    base = struct.unpack_from("<I", exe, pe + 24 + 28)[0]
    out = []
    for i in range(n):
        o = pe + 24 + opt + 40 * i
        vsize, va, rsize, raw = struct.unpack_from("<IIII", exe, o + 8)
        ch = struct.unpack_from("<I", exe, o + 36)[0]
        out.append((exe[o:o + 8].rstrip(b"\0").decode(errors="replace"), base + va,
                    vsize, raw, rsize, ch))
    return base, out


def pe_imports(exe: bytes, base: int, secs) -> list:
    """[(dll name, the address of its first import slot, how many slots)] from the exe FILE. The
    file gives the layout; what the slots HOLD only the running process has, which is the point --
    the code that reaches a slot is stock, so the comparison of the code never looks at it, and
    what makes a slot wrong is where it leads."""
    def off(va):
        for name, sva, vsize, raw, rsize, ch in secs:
            if sva <= va < sva + max(vsize, rsize):
                return raw + (va - sva)
        return None
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    imp = struct.unpack_from("<I", exe, pe + 24 + 104)[0]
    out, o = [], off(base + imp) if imp else None
    if o is None:
        return out
    for i in range(96):                     # a 1997 exe has ten descriptors; this is a bound
        oft, ts, fc, nm, ft = struct.unpack_from("<IIIII", exe, o + 20 * i)
        if not nm and not ft:
            break
        p_nm, p_ft = off(base + nm), off(base + ft)
        if p_nm is None or p_ft is None:
            continue
        name = exe[p_nm:exe.index(b"\0", p_nm)].decode("latin1")
        k = 0
        while p_ft + 4 * (k + 1) <= len(exe) and struct.unpack_from("<I", exe, p_ft + 4 * k)[0]:
            k += 1
        out.append((name, base + ft, k))
    return out


def proc_modules(pid) -> list:
    """(lo, hi, path) of every PE image mapped from a file in the process. /proc/<pid>/maps
    shows a PE image's header page alone, so the extent is the SizeOfImage in that header."""
    span = {}
    for ln in Path(f"/proc/{pid}/maps").read_text().splitlines():
        parts = ln.split(None, 5)
        if len(parts) < 6 or not parts[5].startswith("/"):
            continue
        lo, hi = (int(x, 16) for x in parts[0].split("-"))
        a, b = span.get(parts[5], (lo, hi))
        span[parts[5]] = (min(a, lo), max(b, hi))
    out = []
    with open(f"/proc/{pid}/mem", "rb") as f:
        for path, (lo, _) in span.items():
            try:
                f.seek(lo)
                hdr = f.read(0x400)
                if hdr[:2] != b"MZ":
                    continue
                pe = struct.unpack_from("<I", hdr, 0x3C)[0]
                if pe + 92 > len(hdr) or hdr[pe:pe + 4] != b"PE\0\0":
                    continue
                size = struct.unpack_from("<I", hdr, pe + 24 + 56)[0]
            except (OSError, ValueError, struct.error):
                continue
            if size:
                out.append((lo, lo + size, path))
    return out


def changed_runs(mem: bytes, disk: bytes, va: int) -> list:
    """Every run of bytes that differ, with HOOK_CTX bytes of context each side -- the
    opcode of a changed jump can begin before the first byte that differs, and its operand
    can end after the last."""
    n, i, out = min(len(mem), len(disk)), 0, []
    while i < n:
        if mem[i] == disk[i]:
            i += 1
            continue
        lo = i
        while i < n and mem[i] != disk[i]:
            i += 1
        a, b = max(0, lo - HOOK_CTX), min(n, i + HOOK_CTX)
        out.append({"lo": va + lo, "hi": va + i, "at": va + a,
                    "mem": mem[a:b].hex(), "file": disk[a:b].hex()})
    return out


def decode_run(run, read_dword=None) -> list:
    """(site, target, shaped) for every address a changed run's bytes lead to or hold.

    `shaped` is True for an **instruction that transfers control** and names its target: a rel32
    call or jump (E8, E9), one through a pointer (FF 15, FF 25), `push imm32; ret`, `mov
    eax,imm32; jmp eax`. The indirect form needs a read at an arbitrary address, which only a
    live process gives: without `read_dword` it is left undecoded, which is the Windows watcher's
    one gap.

    The opcode is looked for from five bytes before the first changed byte, because `FF 15`/`FF 25`
    carry their operand at offsets 2 to 5 and a repointed slot address whose last byte alone
    differs begins five bytes back; the other shapes end at offset 4.

    `shaped` is False for four bytes that merely HOLD such a value, at any offset and aligned to
    nothing. Those are a coincidence, not a hook, and nothing is judged on them -- the bytes are
    as likely to be the middle of an instruction or the displacement of a jump: `8B 96 92 00`, the
    middle of a `mov esi,[esi+0x92]` of Impure's, reads as 0x0092968B, and Total Mayhem's recorder
    was mapped at 0x00910000 on the Windows box. Every TADR hook measured on any setup is found by
    its instruction ([the takeover](../../research/notes/compat/takeover.md), part 3)."""
    mem, at, lo, hi = bytes.fromhex(run["mem"]), run["at"], run["lo"], run["hi"]
    out = []
    for s in range(max(at, lo - 5), hi):
        o, n = s - at, len(mem)
        if o < 0 or o >= n:
            continue
        b = mem[o]
        if b in (0xE8, 0xE9) and o + 5 <= n:
            out.append((s, (s + 5 + struct.unpack_from("<i", mem, o + 1)[0]) & 0xFFFFFFFF, True))
        elif b == 0xFF and o + 6 <= n and mem[o + 1] in (0x15, 0x25):
            t = read_dword(struct.unpack_from("<I", mem, o + 2)[0]) if read_dword else None
            if t is not None:
                out.append((s, t, True))
        elif b == 0x68 and o + 6 <= n and mem[o + 5] == 0xC3:
            out.append((s, struct.unpack_from("<I", mem, o + 1)[0], True))
        elif b == 0xB8 and o + 7 <= n and mem[o + 5] == 0xFF and mem[o + 6] == 0xE0:
            out.append((s, struct.unpack_from("<I", mem, o + 1)[0], True))
        if lo <= s and s + 4 <= hi and o + 4 <= n:
            out.append((s, struct.unpack_from("<I", mem, o)[0], False))
    return out


def foreign_hooks(runs, mods, read_dword=None) -> list:
    """Every changed run that leads into one of `mods` -- (lo, hi, name, is_tadr) -- one finding
    a run, as the DLL's own pass reports it. A run is reported by the instruction that goes there
    if it has one, and only otherwise by a value it holds (`shaped` false), which nothing is
    judged on: `judge_hooks` keeps those apart."""
    out = []
    for run in runs:
        hits = [(s, t, sh, m) for s, t, sh in decode_run(run, read_dword)
                for m in [next((m for m in mods if m[0] <= t < m[1]), None)] if m]
        if not hits:
            continue
        # A TADR hit wins the run: the bytes before a run may be a stock `FF 15` through an import
        # slot that leads into the mod's own WIN32.dll, and taking that one would hide a TADR hook
        # in the same run -- the DLL's own pass scans for a TADR target first for that reason.
        site, target, shaped, m = next((h for h in hits if h[2] and h[3][3]),
                                       next((h for h in hits if h[2]), hits[0]))
        out.append({"site": site, "target": target, "module": m[2], "tadr": m[3],
                    "shaped": shaped, "at": run["at"], "mem": run["mem"], "file": run["file"]})
    return out


def descendants(root: int) -> list:
    """`root` and every process under it, from /proc's parent links."""
    kids = {}
    for d in Path("/proc").iterdir():
        if not d.name.isdigit():
            continue
        try:                    # stat's comm is parenthesised and may hold spaces
            kids.setdefault(int((d / "stat").read_text().rsplit(")", 1)[1].split()[1]),
                            []).append(int(d.name))
        except (OSError, IndexError, ValueError):
            continue
    out, todo = [], [root]
    while todo:
        pid = todo.pop()
        out.append(pid)
        todo += kids.get(pid, [])
    return out


def folder_files(gamedir: Path) -> dict:
    """{the path a file of the game folder really is: its name in the folder}. tacli links the
    retail install's own files into an instance instead of copying them, and /proc/<pid>/maps
    names the file a mapping came from, so a module of the folder is found by where its file
    ends up, never by the folder's own path being a prefix of it."""
    out = {}
    try:
        for f in gamedir.iterdir():
            try:
                if f.is_file():
                    out[str(f.resolve()).lower()] = f.name
            except OSError:
                continue
    except OSError:
        pass
    return out


def game_pid(root: int, exe_real: str) -> "int | None":
    """The pid whose memory holds the game. `wine TotalA.exe` maps the PE in the process
    Popen started or in a child of it, so the launcher's pid is not always the game's. Only
    descendants of `root` are looked at: a ptrace_scope of 1 lets a process read its own
    descendants and nothing else, and another session's game must never be read."""
    for pid in descendants(root):
        try:
            if any(ln.lower().endswith(exe_real)
                   for ln in Path(f"/proc/{pid}/maps").read_text().splitlines()):
                return pid
        except OSError:
            continue
    return None


def exe_hooks(root, gamedir: Path) -> dict:
    """Read the game's own code out of the live process and say what it leads into. `root` is
    the pid the runner started; `why` is set when the comparison could not be made at all,
    which is not evidence either way."""
    exe_file = gamedir / "TotalA.exe"
    try:
        exe_real = str(exe_file.resolve()).lower()
        exe = exe_file.read_bytes()
        base, secs = pe_sections(exe)
    except (OSError, IndexError, struct.error) as e:
        return {"why": f"TotalA.exe could not be read as a PE ({e})"}
    pid = game_pid(root, exe_real)
    if pid is None:
        return {"why": f"TotalA.exe is mapped in no process under pid {root}"}
    try:
        mods = proc_modules(pid)
    except OSError as e:
        return {"why": f"the game's memory could not be read ({e}); a parent may read its "
                       f"child under ptrace_scope=1, nobody else"}
    folder, table, image = folder_files(gamedir), [], None
    for lo, hi, path in mods:
        low = path.lower()
        if low == exe_real:
            image = (lo, hi)
            continue
        name = folder.get(low)
        if not name or name.lower() == "ddraw.dll":
            continue
        try:
            tadr = TADR_MARK in Path(path).read_bytes()
        except OSError:
            tadr = False
        table.append((lo, hi, name, tadr))
    if image is None:
        return {"why": f"TotalA.exe is not mapped in pid {pid}"}
    if image[0] != base:
        return {"why": f"the exe is mapped at 0x{image[0]:08X}, not the 0x{base:08X} its file "
                       f"asks for: every byte would differ"}
    runs, sections = [], 0
    try:
        with open(f"/proc/{pid}/mem", "rb") as f:
            for name, va, vsize, raw, rsize, ch in secs:
                n = min(vsize, rsize)
                if not ch & CODE_FLAGS or not n or raw + n > len(exe):
                    continue
                f.seek(va)
                mem = f.read(n)
                if len(mem) != n:
                    return {"why": f"only {len(mem)} of {n} bytes of {name} could be read"}
                sections += 1
                runs += changed_runs(mem, exe[raw:raw + n], va)
    except OSError as e:
        return {"why": f"the game's code could not be read ({e})"}

    def read_dword(a):
        try:
            with open(f"/proc/{pid}/mem", "rb") as g:
                g.seek(a)
                d = g.read(4)
            return struct.unpack("<I", d)[0] if len(d) == 4 else None
        except OSError:
            return None

    slots = []
    try:
        with open(f"/proc/{pid}/mem", "rb") as f:
            for dll, addr, n in pe_imports(exe, base, secs):
                f.seek(addr)
                got = f.read(4 * n)
                if len(got) != 4 * n:
                    slots = None
                    break
                slots += [(dll, addr + 4 * k, v)
                          for k, v in enumerate(struct.unpack(f"<{n}I", got))]
    except (OSError, struct.error):
        slots = None
    out = judge_hooks(runs, table, sections, read_dword, slots)
    out["pid"] = pid
    return out


def import_slots(slots, table) -> list:
    """Every import slot of the exe that leads somewhere surprising. `slots` is [(dll, address,
    bound value)] read out of the running process, `dll` the descriptor's own name.

    A slot bound into the very DLL its descriptor names is not a change at all -- the retail exe's
    own imports of WIN32.dll and smackw32.DLL are 16 such slots on every setup -- so those are left
    out unless that DLL is TADR's, which is exactly the recorder installed as the game folder's
    dplayx.dll. What is left is a slot leading somewhere the file did not ask for."""
    out = []
    for dll, addr, target in slots:
        m = next((m for m in table if m[0] <= target < m[1]), None)
        if not m or (not m[3] and m[2].lower() == dll.lower()):
            continue
        out.append({"dll": dll, "site": addr, "target": target, "module": m[2], "tadr": m[3]})
    return out


def judge_hooks(runs, table, sections, read_dword=None, slots=None) -> dict:
    """The verdict both platforms share, from the changed runs, the module table and -- where the
    reader can give them -- the exe's bound import slots."""
    hooks = foreign_hooks(runs, table, read_dword)
    iat = import_slots(slots, table) if slots is not None else []
    return {"why": None, "runs": len(runs), "sections": sections,
            "slots_read": slots is not None, "slots": iat,
            "modules": [f"{m[2]} 0x{m[0]:08X}-0x{m[1]:08X}{' TADR' if m[3] else ''}"
                        for m in table],
            "tadr": [h for h in hooks if h["tadr"] and h["shaped"]]
                    + [h for h in iat if h["tadr"]],
            "other": [h for h in hooks if not h["tadr"] and h["shaped"]]
                     + [h for h in iat if not h["tadr"]],
            "holds": [h for h in hooks if not h["shaped"]]}


def win_hooks(ev) -> dict:
    """win-watch.ps1's `code` event turned into the verdict exe_hooks gives on Wine: the
    watcher dumps the changed runs and the module table, and the decode is the same one
    (decode_run). Its one gap is the indirect call form, which needs a read at an arbitrary
    address the watcher does not make."""
    def many(v):
        # ConvertTo-Json gives a bare object, not a list, for a one-element array
        return [] if v is None else v if isinstance(v, list) else [v]

    c = (ev or {}).get("code") or {}
    if not c:
        return {"why": "the watcher wrote no code event"}
    if c.get("why"):
        return {"why": c["why"]}
    table = [(int(m["base"]), int(m["base"]) + int(m["size"]), m["name"], bool(m["tadr"]))
             for m in many(c.get("modules"))]
    runs = []
    for rec in many(c.get("runs")):
        kind, at, lo, hi, mem, file = rec.split("|")
        if kind == "run":
            runs.append({"at": int(at, 16), "lo": int(lo, 16), "hi": int(hi, 16),
                         "mem": mem, "file": file})
    return judge_hooks(runs, table, int(c.get("sections") or 0))


def hook_evidence(h, where="") -> list:
    """What reading the process says about TADR's code having run. A comparison that could
    not be made is NOT listed here -- it is not evidence of TADR -- it fails the run's goal
    through judge() instead, so a goal of `tadr_ran: false` is never met by a check that
    did not happen."""
    if not h or h.get("why"):
        return []
    by = {}
    for x in h["tadr"]:
        by.setdefault(x["module"] + (" (an import slot of the exe)" if x.get("dll") else ""),
                      []).append(x)
    return [f"{where}the game's code leads into {name} at "
            + ", ".join(f"0x{x['site']:08X}" for x in sites[:6])
            + (f" and {len(sites) - 6} more" if len(sites) > 6 else "")
            for name, sites in sorted(by.items())]


def hook_note(h) -> str:
    """One line for the run's report."""
    if not h:
        return "the game's code was not compared with its file"
    if h.get("why"):
        return f"the game's code was not compared with its file: {h['why']}"
    return (f"{h['runs']} changed runs in {h['sections']} executable section(s), "
            f"{len(h['tadr'])} into TADR and {len(h['other'])} into another DLL of the "
            f"folder, of {len(h['modules'])} looked at; {len(h.get('holds', []))} hold such an "
            f"address with no instruction that goes there, which is not counted"
            + ("" if h.get("slots_read") else "; the exe's import slots were not read"))


def run_wine(setup, dll, watch, display, keep_screens, battle=0) -> dict:
    inst = setup["_inst"]
    gamedir, prefix = inst["gamedir"], inst["prefix"]
    env = dict(os.environ, WINEPREFIX=str(prefix), DISPLAY=f":{display}",
               WINEDLLOVERRIDES=f"ddraw=n,b;{DPLAY_OVERRIDES};{AUDIO_OFF}",
               ALSA_CONFIG_PATH=str(ASOUND_NULL), WINEDEBUG="+loaddll")
    xv = start_xvfb(display)
    log = tempfile.NamedTemporaryFile(prefix="tacompat-wine-", suffix=".log", delete=False)
    t0 = time.time()
    proc = subprocess.Popen(["wine", "TotalA.exe"], cwd=str(gamedir), env=env, stdout=log,
                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                            start_new_session=True)
    boxes, seen, menu, shots = [], set(), None, {}
    first_box_at = exited_at = None
    impure_log = gamedir / "log" / "tagpu.log"
    try:
        while time.time() - t0 < watch:
            time.sleep(1.0)
            labelled = impure_log.exists() and impure_log.stat().st_mtime >= t0
            for title, w, h in x_windows(display):
                if main_window(title, w, h):
                    continue
                if (title, w, h) not in seen:
                    seen.add((title, w, h))
                    boxes.append({"title": title, "size": f"{w}x{h}", "t": round(time.time() - t0, 1)})
                    if first_box_at is None:
                        first_box_at = time.time()
                        shot = Path(log.name).with_suffix(".box.png")
                        subprocess.run(["import", "-display", f":{display}", "-window", "root",
                                        str(shot)], capture_output=True, timeout=20)
                        shots["box.png"] = str(shot)
            if proc.poll() is not None and exited_at is None:
                exited_at = time.time()
            if exited_at and time.time() - exited_at > 2:
                break
            if first_box_at and time.time() - first_box_at > 3:
                break
            if menu is None and labelled and time.time() - t0 > 8 and not boxes:
                r = tacli("ui", inst["name"], timeout=15)
                first = (r.stdout.splitlines() or [""])[0]
                if r.returncode == 0 and "MAINMENU" in first:
                    menu = True
        content = mod_content(setup, inst) if menu and proc.poll() is None else None
        fight = None
        if battle and menu and not boxes and proc.poll() is None:
            fight = wine_battle(inst, proc, display, gamedir, seen, boxes, t0, battle)
        # The last moment the game is alive: its own code, read from outside (exe_hooks).
        # A game that refused or crashed leaves no process to read, and no claim either.
        hooks = exe_hooks(proc.pid, gamedir) if proc.poll() is None else None
        if keep_screens and "box.png" not in shots:
            shot = Path(log.name).with_suffix(".end.png")
            subprocess.run(["import", "-display", f":{display}", "-window", "root", str(shot)],
                           capture_output=True, timeout=20)
            shots["end.png"] = str(shot)
    finally:
        alive = proc.poll() is None
        subprocess.run(["wineserver", "-k"], env=env, capture_output=True, timeout=30)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
        stop_xvfb(xv)
        log.close()
    wlog = Path(log.name).read_text(errors="replace")
    # +loaddll names a module as L"Z:\\home\\...\\gamedir\\x.dll" (backslashes doubled):
    # the game folder's own modules are the ones under this instance's gamedir.
    here = f"\\instances\\{inst['name']}\\gamedir\\"
    folder_modules = sorted({path for path in (m.group(1).replace("\\\\", "\\") for m in
                             re.finditer(r'Loaded L"([^"]+)" at [0-9A-Fa-f]+: native', wlog))
                             if here in path.lower()})

    def read(p):
        return p.read_text(errors="replace") if p.exists() else None

    tagpu = read(impure_log) if impure_log.exists() and impure_log.stat().st_mtime >= t0 else None
    o = {
        "setup": setup["name"], "platform": "wine",
        "boxes": boxes, "alive_at_end": alive,
        "exit_code": None if alive else proc.returncode,
        "impure_loaded": tagpu is not None,
        "packet_pub": packet_pub(tagpu or ""),
        "menu": menu if tagpu is not None else None,
        "errorlog": read(gamedir / "ErrorLog.txt"),
        "tdrawlog": read(gamedir / "tdrawlog.txt"),
        "failure": read(gamedir / "log" / "startup-failure.txt"),
        "modules_known": True, "folder_modules": folder_modules,
        "battle": fight, "hooks": hooks, "content": content,
        "seconds": round(time.time() - t0, 1),
    }
    logs = other_logs(gamedir / "log", t0)
    o["tadr_ran"] = tadr_evidence(o["tdrawlog"], logs) + hook_evidence(hooks)
    o["outcome"] = classify(o)
    o["gui"] = gui_health(tagpu or "")
    files = {"wine.log": wlog, **{f"log-{k}": v for k, v in logs.items()}}
    for k in ("tdrawlog", "failure", "errorlog"):
        if o[k]:
            files[{"tdrawlog": "tdrawlog.txt", "failure": "startup-failure.txt",
                   "errorlog": "ErrorLog.txt"}[k]] = o[k]
    if tagpu:
        files["tagpu.log"] = tagpu
    o["_files"], o["_copies"] = files, shots
    os.unlink(log.name)
    return o


# ------------------------------------------------------------------------- Wine, two players

class Lobby(Exception):
    """A step of the two-player start that did not happen, in words."""


def mp_eligible(setup) -> bool:
    """A player's setup whose goal is Impure running: a harness setup (one with a lever)
    checks a mechanism, not a game, and a setup whose battle room this walk cannot start a game
    in says so in `no_network_game` -- with what it does instead, measured."""
    return (setup["goal"]["outcome"] == "impure-active" and not setup.get("levers")
            and not setup.get("no_network_game"))


def dplay_holders(port) -> list:
    """(pid, WINEPREFIX) of every process listening on `port`, UDP or TCP."""
    r = subprocess.run(["ss", "-tulnpH", f"sport = :{port}"], capture_output=True, text=True,
                       timeout=10)
    out = []
    for pid in sorted(set(re.findall(r"pid=(\d+)", r.stdout))):
        try:
            env = Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
        except OSError:
            continue
        prefix = next((e[11:].decode(errors="replace") for e in env if e.startswith(b"WINEPREFIX=")), "")
        out.append((int(pid), prefix))
    return out


def free_dplay_port(port, mine: set, wait=300) -> "str | None":
    """Make `port` free for our host, or say who holds it. A holder in a prefix THIS run
    created is ours and stale, and its wineserver is ended; anything else is someone else's
    game, waited for and never touched. The instance names are fixed, so another worktree's
    session runs prefixes named exactly like ours and only the set we built may be ended
    (parallel-mp-runs-share-dplay-port). Both branches share one deadline: a holder of ours
    that will not go is a failure to report, not a loop to sit in."""
    deadline = time.time() + wait
    while True:
        holders = dplay_holders(port)
        if not holders:
            return None
        ours = [pf for _, pf in holders if pf and str(Path(pf)) in mine]
        for pf in ours:
            subprocess.run(["wineserver", "-k"], env=dict(os.environ, WINEPREFIX=pf),
                           capture_output=True, timeout=30)
        if time.time() > deadline:
            return ", ".join(f"pid {pid} ({pf or 'no WINEPREFIX'})"
                             + (" -- this run's, and it would not go" if pf and str(Path(pf)) in mine else "")
                             for pid, pf in holders)
        time.sleep(2 if ours else 5)


def start_wine(inst, display):
    """TotalA.exe in the instance's game folder on its own virtual display."""
    env = dict(os.environ, WINEPREFIX=str(inst["prefix"]), DISPLAY=f":{display}",
               WINEDLLOVERRIDES=f"ddraw=n,b;{DPLAY_OVERRIDES};{AUDIO_OFF}",
               ALSA_CONFIG_PATH=str(ASOUND_NULL), WINEDEBUG="+loaddll")
    xv = start_xvfb(display)
    log = tempfile.NamedTemporaryFile(prefix="tacompat-wine-", suffix=".log", delete=False)
    proc = subprocess.Popen(["wine", "TotalA.exe"], cwd=str(inst["gamedir"]), env=env, stdout=log,
                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                            start_new_session=True)
    return {"inst": inst, "display": display, "env": env, "xv": xv, "log": log, "proc": proc}


def stop_wine(g):
    subprocess.run(["wineserver", "-k"], env=g["env"], capture_output=True, timeout=30)
    try:
        g["proc"].wait(timeout=15)
    except subprocess.TimeoutExpired:
        g["proc"].kill()
    stop_xvfb(g["xv"])
    g["log"].close()
    os.unlink(g["log"].name)


def lobby_ui(inst, *argv, timeout=40) -> str:
    # A SCREEN STILL COMING UP IS NOT A FAILURE. Between two of the battle room's screens there
    # is a moment with no gadget layer at all, and a mod with more content than stock stays in it
    # longer than one call's patience (MEASURED 2026-09-27: Escalation's host on START). Retried
    # while that is what it says, and no longer.
    end, last = time.time() + 40, ""
    while True:
        r = tacli("ui", inst, *argv, "--timeout", str(timeout), timeout=timeout + 30)
        if r.returncode == 0:
            return r.stdout
        last = (r.stderr or r.stdout).strip()[:200]
        if time.time() >= end or "no active gui" not in last:
            raise Lobby(f"{inst}: ui {' '.join(argv)}: {last}")
        time.sleep(3)


def lobby_field(inst, name, value):
    """Click, then fill -- an unfocused field takes the text as quickkeys -- and leave a
    field alone that already reads the value (tools/mp_lobby.sh has the story)."""
    cur = re.search(r"^text\s+(.*)$", lobby_ui(inst, "show", name), re.M)
    if cur and cur.group(1).strip() == value:
        return
    lobby_ui(inst, "click", name)
    lobby_ui(inst, "fill", name, value)


def lobby_to_selgame(inst, nick):
    lobby_ui(inst, "click", "MULTI")
    lobby_ui(inst, "select", "DPLAY", "Internet TCP/IP Connection For DirectPlay")
    lobby_ui(inst, "click", "SELECT", timeout=25)
    lobby_field(inst, "ADDRESS", "127.0.0.1")
    lobby_ui(inst, "click", "OK", timeout=30)
    lobby_ui(inst, "wait", "--gui", "SELGAME", timeout=30)
    lobby_field(inst, "NICKNAME", nick)


def wine_lobby(host, join, port):
    """tools/mp_lobby.sh's walk, host then joiner, into one live game on the battle's map."""
    lobby_to_selgame(host, "HOST")
    lobby_ui(host, "click", "STARTNEW", timeout=30)
    lobby_field(host, "GAMENAME", "COMPAT")
    lobby_field(host, "NICKNAME", "HOST")
    lobby_ui(host, "click", "OK", timeout=30)
    lobby_ui(host, "wait", "--gui", "LOUNGE2", timeout=30)
    lobby_ui(host, "click", "MAP", timeout=25)
    lobby_ui(host, "select", "MAPNAMES", BATTLE_MAP, timeout=120)
    lobby_ui(host, "click", "LOAD", timeout=25)
    lobby_to_selgame(join, "JOIN")
    # The session list is filled when SELGAME opens and again on UPDATE, never by itself: a
    # host still answering its map load when the joiner looked is missing until someone
    # presses UPDATE, as a player would (seen with Total Mayhem 11.3.0, 2026-09-27).
    # Twelve rounds, not six: a mod's host is still answering its own map load well past the
    # half-minute stock TA needs (MEASURED 2026-09-27: Escalation's host, where JOINGAME was
    # still grey after six).
    for _ in range(12):
        if re.search(r"^grayed\s+0\s*$", lobby_ui(join, "show", "JOINGAME"), re.M):
            break
        lobby_ui(join, "click", "UPDATE")
        time.sleep(5)
    else:
        # Say who answers the joiner's enumeration: the host's own name server, a stale one of
        # another prefix, or nobody -- three different failures that read the same on screen.
        raise Lobby(f"{join}: no session listed after 12 UPDATEs; DirectPlay port {port} held by "
                    f"{dplay_holders(port) or 'nobody'}")
    lobby_ui(join, "click", "JOINGAME", timeout=30)
    lobby_ui(join, "wait", "--gui", "LOUNGE2", timeout=30)
    lobby_ui(join, "click", "READY0", timeout=20)     # each client lists itself as row 0
    lobby_ui(host, "click", "READY0", timeout=20)
    # THE ORACLE FOR START IS THE GAME COMING ALIVE, NOT THE CLICK'S ANSWER. The click that
    # starts the game takes away the gadget layer it was read from, so a click that worked can
    # still answer "no active gui" -- or, one patient retry later, name the in-game panel
    # (MEASURED 2026-09-27: Escalation's host, ARMMAIN2.GUI, twice). The wait below decides.
    try:
        lobby_ui(host, "click", "START", timeout=30)
    except Lobby as e:
        if not re.search(r"no active gui|ARMMAIN|CORMAIN", str(e)):
            raise
    for inst in (host, join):
        r = tacli("wait", inst, "alive=[1-9]", "--timeout", "150", timeout=200)
        if r.returncode != 0:
            raise Lobby(f"{inst}: the game never came alive: {(r.stderr or r.stdout).strip()[:200]}")


CREATES = threading.Lock()       # `tacli create` writes the hive every prefix shares: prepare_wine


def run_wine_mp(setup, dll, seconds, taken, mine, ports) -> dict:
    """Two players, the setup's folder on each, hosted and joined through the game's own
    battle room over Windows' DirectPlay, a small fight between them, then watched. Every
    peer's folder is read for TADR's evidence, and every peer's process for its code.

    A PORT OF ITS OWN. The game takes a DirectPlay port from `ports` and puts it into both
    peers' prefixes, so its name server and its joiner's enumeration meet on that port and on
    no other game's: as many games run at once as `ports` holds, and the port goes back when
    the game is over. The host peer's single-player run has ended by now (this is queued from
    its result), and prepare_wine has just installed the joiner's stock DirectPlay, so the
    patch lands on files no process of the game has open yet.

    The displays are taken HERE, not when this was queued: a number reserved while the
    single-player runs were still going is one no Xvfb held for minutes, and free_display
    counts a stale lock as a display in use."""
    create_display, host_display, join_display = (free_display(taken) for _ in range(3))
    xv = start_xvfb(create_display)
    try:
        with CREATES:
            join = prepare_wine(setup, dll, create_display, suffix="-j")
    finally:
        stop_xvfb(xv)
    mine.add(str(join["prefix"]))
    port = ports.get()
    try:
        for inst in (setup["_inst"], join):
            dpport.set_port(inst["prefix"], port)
        held = free_dplay_port(port, mine)
        if held:
            return {"ok": False, "why": f"could not run: DirectPlay's port {port} is held by {held}",
                    "evidence": [], "port": port}
        out = play_mp(setup["_inst"], join, seconds, host_display, join_display, port)
        out["port"] = port
        return out
    except SystemExit as e:                     # dpport refused the files: say so, as a result
        return {"ok": False, "why": f"could not run: {e}", "evidence": [], "port": port}
    finally:
        ports.put(port)


def play_mp(host, join, seconds, host_display, join_display, port) -> dict:
    """The game itself: both peers started, walked into one game, fought, watched, read."""
    peers = {"host": host, "join": join}
    t0 = time.time()
    games = {}
    boxes, seen, why = [], set(), None
    shots, hooks = {}, {}
    try:
        try:
            games["host"] = start_wine(peers["host"], host_display)
            time.sleep(3)
            games["join"] = start_wine(peers["join"], join_display)
            deadline = time.time() + 60
            menus = set()
            while len(menus) < 2 and time.time() < deadline and not boxes:
                time.sleep(2)
                for role, g in games.items():
                    new_boxes(g["display"], seen, boxes, t0)
                    if role not in menus:
                        first = (tacli("ui", g["inst"]["name"], timeout=15).stdout.splitlines() or [""])[0]
                        if "MAINMENU" in first:
                            menus.add(role)
            if boxes:
                raise Lobby(f"a box before the menu: {boxes[0]['title']}")
            if len(menus) < 2:
                raise Lobby(f"the main menu never came up on {', '.join(sorted(set(games) - menus))}")
            wine_lobby(peers["host"]["name"], peers["join"]["name"], port)
            for role, scen in zip(("host", "join"), MP_SCENARIOS):
                r = tacli("scenario", "apply", peers[role]["name"], scen, timeout=180)
                if r.returncode != 0:
                    raise Lobby(f"{role}: scenario apply {scen}: {(r.stderr or r.stdout).strip()[:200]}")
            end = time.time() + seconds
            while time.time() < end and not why:
                time.sleep(2)
                for role, g in games.items():
                    new_boxes(g["display"], seen, boxes, t0)
                    if g["proc"].poll() is not None:
                        why = f"the {role}'s game exited during the fight"
                    elif (peers[role]["gamedir"] / "ErrorLog.txt").exists():
                        why = f"the {role}'s game wrote a crash report"
                if boxes and not why:
                    why = f"a box during the fight: {boxes[0]['title']}"
            for role, g in games.items():        # while the games are still up (exe_hooks)
                if g["proc"].poll() is None:
                    hooks[role] = exe_hooks(g["proc"].pid, peers[role]["gamedir"])
        # A step that throws is this game's failure, not the suite's: tacli can time out,
        # an Xvfb or an instance can be gone, and die() raises SystemExit. The report is
        # worth more than the traceback.
        except (Lobby, subprocess.SubprocessError, OSError, SystemExit) as e:
            why = f"{type(e).__name__}: {e}" if not isinstance(e, Lobby) else str(e)
        if why:                         # what each screen showed when the step failed
            for role, g in games.items():
                shot = Path(g["log"].name).with_suffix(f".mp-{role}.png")
                subprocess.run(["import", "-display", f":{g['display']}", "-window", "root", str(shot)],
                               capture_output=True, timeout=20)
                shots[f"mp-{role}.png"] = str(shot)
    finally:
        for g in games.values():
            stop_wine(g)
    out = {"ok": False, "boxes": boxes, "evidence": [], "_files": {}, "_copies": shots}
    for role, inst in peers.items():
        gd = inst["gamedir"]
        tlog = gd / "log" / "tagpu.log"
        tagpu = tlog.read_text(errors="replace") if tlog.exists() and tlog.stat().st_mtime >= t0 else ""
        td = gd / "tdrawlog.txt"
        tdrawlog = td.read_text(errors="replace") if td.exists() and td.stat().st_mtime >= t0 else None
        logs = other_logs(gd / "log", t0)
        out[role] = {"packet_pub": packet_pub(tagpu), "impure_loaded": bool(tagpu)}
        out["evidence"] += (tadr_evidence(tdrawlog, logs, where=f"network game, {role}: ")
                            + hook_evidence(hooks.get(role), where=f"network game, {role}: "))
        out[role]["hooks"] = hooks.get(role)
        out["_files"].update({f"mp-{role}-tagpu.log": tagpu,
                              **{f"mp-{role}-log-{k}": v for k, v in logs.items()}})
        err = gd / "ErrorLog.txt"
        if err.exists() and err.stat().st_mtime >= t0:
            out["_files"][f"mp-{role}-ErrorLog.txt"] = err.read_text(errors="replace")
        if not why and not out[role]["packet_pub"]:
            why = f"Impure drew nothing on the {role}'s game"
    out["ok"] = why is None
    out["why"] = why or f"two players, {MP_SCENARIOS[0]} and {MP_SCENARIOS[1]} applied, {seconds} s"
    out["seconds"] = round(time.time() - t0, 1)
    return out


def threw(setup, platform, fut) -> dict:
    """A run's result, or -- when the run itself threw -- a `no-result` of its own. One setup
    that falls over must not cost the report on all the others; the exception is the row."""
    try:
        return fut.result()
    except BaseException as e:
        return {"setup": setup["name"], "platform": platform, "boxes": [], "alive_at_end": False,
                "exit_code": None, "impure_loaded": False, "packet_pub": 0, "menu": None,
                "errorlog": None, "tdrawlog": None, "failure": None, "modules_known": False,
                "folder_modules": [], "battle": None, "hooks": None, "tadr_ran": [],
                "outcome": "no-result", "why": f"{type(e).__name__}: {e}", "seconds": 0,
                "_files": {}, "_copies": {}}


def cmd_wine(args):
    source = Path(args.dll).resolve()
    if not source.is_file():
        die(f"no DLL at {source} (build it: make -C tagpu/ddraw -j$(nproc))")
    dll = snapshot_dll(source)
    setups = pick_setups(args.setups)
    ready = [s for s in setups if not any(fixture_problems(a["fixture"]) for a in s["add"])]
    skipped = [s["name"] for s in setups if s not in ready]
    if skipped:
        print(f"skipped, fixtures not fetched: {', '.join(skipped)} (tacompat.py list)")
    if not ready:
        die("nothing to run")
    started = time.time()
    taken = set()
    create_display = free_display(taken)
    xv = start_xvfb(create_display)
    try:
        for s in ready:                     # one at a time: see prepare_wine
            print(f"preparing {s['name']}")
            s["_inst"] = prepare_wine(s, dll, create_display)
    finally:
        stop_xvfb(xv)
    displays = {s["name"]: free_display(taken) for s in ready}
    # Every prefix this run made, for free_dplay_port: the only ones it may end.
    mine = {str(s["_inst"]["prefix"]) for s in ready}
    print(f"running {len(ready)} setups, {args.jobs} at a time, {args.watch} s each"
          + (f"; a two-player game of {args.mp} s for each that starts" if args.mp else ""))
    results, mp_futs = [], {}
    # Each network game is queued the moment its setup's single-player run shows Impure
    # running, beside the other runs, and `--mp-jobs` play at once -- one DirectPlay port each.
    mp_jobs = max(1, args.mp_jobs)
    ports = queue.Queue()
    for k in range(mp_jobs):
        ports.put(MP_PORT_BASE + k)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool, \
            concurrent.futures.ThreadPoolExecutor(max_workers=mp_jobs) as mp_pool:
        futs = {pool.submit(run_wine, s, dll, args.watch, displays[s["name"]], args.screens,
                            args.battle): s
                for s in ready}
        for f in concurrent.futures.as_completed(futs):
            s = futs[f]
            o = threw(s, "wine", f)         # a run that threw is its own result, and reported
            verdict(o, s, "wine")
            print(f"  {s['name']}: {o['outcome']} ({o['verdict']})")
            results.append(o)
            if s.get("no_network_game"):
                o["mp_skip"] = s["no_network_game"]
            if args.mp and mp_eligible(s) and o["outcome"] == "impure-active" and o.get("menu"):
                mp_futs[s["name"]] = (s, o, mp_pool.submit(run_wine_mp, s, dll, args.mp,
                                                           taken, mine, ports))
        for name, (s, o, f) in mp_futs.items():
            try:
                m = f.result()
            except BaseException as e:
                m = {"ok": False, "why": f"{type(e).__name__}: {e}", "evidence": []}
            o["_files"].update(m.pop("_files", {}))
            o["_copies"].update(m.pop("_copies", {}))
            o["tadr_ran"] = o.get("tadr_ran", []) + m["evidence"]
            o["mp"] = m
            verdict(o, s, "wine")
            print(f"  {name}: network game {'ok' if m['ok'] else 'FAILED: ' + m['why']} ({o['verdict']})")
    results.sort(key=lambda r: [s["name"] for s in SETUPS].index(r["setup"]))
    rc = report(results, "wine", dll, started, args.strict, source)
    shutil.rmtree(dll.parent, ignore_errors=True)
    return rc


def cmd_clean(args):
    for d in sorted(INSTANCES.glob(PREFIX + "*")):
        if (d / "instance.json").exists():
            tacli("rm", d.name, "--force")
            print(f"removed {d.name}")
    return 0


# ------------------------------------------------------------------------- Windows

# Folders on the machine, under the SSH user's profile. `base` is the player's folder
# copied once and never launched; `work` is rebuilt from it for every setup (robocopy
# /MIR copies only what differs), so a setup never sees the previous one's files.
WIN_ROOT = "ta-compat"
WIN_GAME_TASK = "\\tacli\\compat-game"
WIN_WATCH_TASK = "\\tacli\\compat-watch"
# Files a player's folder may carry that would change which route a setup takes: none
# of them reaches `base`, and each setup puts back only what it names.
WIN_EXCLUDE = ("ddraw.dll", "dplayx.dll", "tdraw.dll", "tplayx.dll", "zplayx.dll", "eplayx.dll",
               "TAESC.dll", "emusi.dll", "tmusi.dll", "ddraw_custom.dll", "ddraw.ini", "impure.cfg",
               "impure.cfg.tmp", "tdrawlog.txt", "ErrorLog.txt")
TASK_RUNNING = 0x41301
ps = taremote.ps_str


class Win:
    def __init__(self, cfg):
        self.cfg = cfg
        self.s = taremote.Session(cfg["ssh"], cfg.get("key"))
        self.profile = self.one("Write-Output $env:USERPROFILE")
        self.root = ntpath.join(self.profile, WIN_ROOT)
        # The 32-bit PowerShell: a 64-bit one lists only the exe and the WOW64 layer among a
        # 32-bit process's modules, never the DLLs it loaded from the game folder.
        self.powershell32 = ntpath.join(self.one("Write-Output $env:windir"), "SysWOW64",
                                        "WindowsPowerShell", "v1.0", "powershell.exe")

    def run(self, statements, timeout=120.0):
        return self.s.run(statements, timeout=timeout)

    def one(self, statement, timeout=60.0) -> str:
        out = self.run([statement], timeout=timeout)
        return out[-1].strip() if out else ""

    def path(self, *parts) -> str:
        return ntpath.join(self.root, *parts)

    def md5(self, win_path) -> str:
        return self.one(f"if (Test-Path -LiteralPath {ps(win_path)}) {{ Write-Output (Get-FileHash "
                        f"-Algorithm MD5 -LiteralPath {ps(win_path)}).Hash.ToLower() }} else "
                        f"{{ Write-Output 'absent' }}")

    def upload(self, local: Path, win_path: str):
        """scp, not the session: an archive of 100 MB would cross the PowerShell link one
        statement a megabyte. The target folder must exist first."""
        self.run([f"New-Item -ItemType Directory -Force -Path {ps(ntpath.dirname(win_path))} | Out-Null"])
        dest = win_path.replace("\\", "/")
        argv = ["scp", "-q", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15"]
        if self.cfg.get("key"):
            argv += ["-i", str(self.cfg["key"]), "-o", "IdentitiesOnly=yes"]
        subprocess.run(argv + [str(local), f"{self.cfg['ssh']}:{dest}"], check=True, timeout=1800)

    def read_b64(self, win_path):
        out = self.run([f"if (Test-Path -LiteralPath {ps(win_path)}) {{ Write-Output ([Convert]::"
                        f"ToBase64String([IO.File]::ReadAllBytes({ps(win_path)}))) }}"], timeout=120)
        if not out:
            return None
        import base64
        # utf-8-sig: Windows PowerShell 5 starts a file it writes as UTF8 with a byte-order
        # mark, and a first line that does not start with "{" is not read as an event.
        return base64.b64decode("".join(out)).decode("utf-8-sig", "replace")

    def game_procs(self) -> list:
        return self.run(["Get-Process TotalA -ErrorAction SilentlyContinue | ForEach-Object "
                         "{ Write-Output ('' + $_.Id + '|' + $_.Path) }"])

    def task(self, name, execute, argument, workdir):
        path, _, leaf = name.rpartition("\\")
        user = self.one("Write-Output (Get-CimInstance Win32_ComputerSystem).UserName")
        if not user:
            die("nobody is logged on to the machine's desktop: the game has nowhere to show")
        arg = f" -Argument {ps(argument)}" if argument else ""
        self.run([
            f"$a = New-ScheduledTaskAction -Execute {ps(execute)} -WorkingDirectory {ps(workdir)}{arg}",
            f"$pr = New-ScheduledTaskPrincipal -UserId {ps(user)} -LogonType Interactive",
            "$st = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) "
            "-AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -Priority 4",
            f"Register-ScheduledTask -TaskPath {ps(path + chr(92))} -TaskName {ps(leaf)} -Action $a "
            f"-Principal $pr -Settings $st -Force | Out-Null",
            f"Start-ScheduledTask -TaskPath {ps(path + chr(92))} -TaskName {ps(leaf)}"])

    def task_result(self, name):
        path, _, leaf = name.rpartition("\\")
        r = self.one(f"$t = Get-ScheduledTask -TaskPath {ps(path + chr(92))} -TaskName {ps(leaf)} "
                     f"-ErrorAction SilentlyContinue; if ($t) {{ Write-Output ('' + ($t | "
                     f"Get-ScheduledTaskInfo).LastTaskResult) }}")
        return int(r) & 0xFFFFFFFF if r.lstrip("-").isdigit() else None

    def task_remove(self, name):
        path, _, leaf = name.rpartition("\\")
        self.run([f"$t = Get-ScheduledTask -TaskPath {ps(path + chr(92))} -TaskName {ps(leaf)} "
                  f"-ErrorAction SilentlyContinue; if ($t) {{ Unregister-ScheduledTask -TaskPath "
                  f"{ps(path + chr(92))} -TaskName {ps(leaf)} -Confirm:$false }}"])

    def robocopy(self, src, dst, extra=""):
        """robocopy's exit code is a bit set: below 8 is success, 8 and up a failure."""
        self.run([f"$null = robocopy {ps(src)} {ps(dst)} /MIR /NFL /NDL /NJH /NJS /NP /R:1 /W:1{extra}; "
                  f"if ($LASTEXITCODE -ge 8) {{ throw ('robocopy exit ' + $LASTEXITCODE) }}"], timeout=900)


def stop_work_games(w: Win, work):
    """Stop every game started from the work folder, and wait until each has exited:
    Stop-Process returns before the process lets go of its files, and the DLL's log is
    open without read sharing until then."""
    for line in w.game_procs():
        pid, _, path = line.partition("|")
        if path.lower().startswith(work.lower() + "\\") and pid.isdigit():
            w.run([f"Stop-Process -Id {int(pid)} -Force -ErrorAction SilentlyContinue",
                   f"Wait-Process -Id {int(pid)} -Timeout 30 -ErrorAction SilentlyContinue"], timeout=60)


def fresh_work(w: Win) -> str:
    """The work folder, at a path Windows has never started TotalA.exe from.

    A PLAYER'S FIRST LAUNCH IS NOT A LATER ONE. Windows' compatibility engine hooks the exe's
    DirectDrawCreate import (apphelp.dll) on the first start of an exe from a path it has not
    seen, and not on the next -- measured 2026-09-27, and it is what the takeover got wrong in
    v0.3 on a fresh install while this suite, reusing one folder, passed. So every setup runs
    from a new path: the last work folder is renamed (instant) and mirrored from base (only what
    differs is copied), and nothing older is kept."""
    root = w.path()
    new = w.path(f"work-{time.strftime('%Y%m%d%H%M%S')}-{time.time_ns() % 1000000:06d}")
    w.run([f"$old = @(Get-ChildItem -LiteralPath {ps(root)} -Directory | Where-Object "
           "{ $_.Name -eq 'work' -or $_.Name -like 'work-*' } | Sort-Object LastWriteTime)",
           "if ($old.Count) { $keep = $old[-1]; $old | Where-Object { $_.FullName -ne "
           "$keep.FullName } | Remove-Item -Recurse -Force; "
           f"Rename-Item -LiteralPath $keep.FullName -NewName {ps(ntpath.basename(new))} }}"],
          timeout=600)
    w.work = new
    return new


def run_windows_setup(w: Win, setup, watch) -> dict:
    work, base = fresh_work(w), w.path("base")
    w.robocopy(base, work)
    for dest, src in overlay(setup):
        rel = src.relative_to(FIXTURES)
        if len(Path(dest).parts) > 1:
            w.run([f"New-Item -ItemType Directory -Force -Path "
                   f"{ps(ntpath.join(work, *Path(dest).parts[:-1]))} | Out-Null"])
        w.run([f"Copy-Item -LiteralPath {ps(w.path('fixtures', *rel.parts))} -Destination "
               f"{ps(ntpath.join(work, *Path(dest).parts))} -Force"])
    w.run([f"Copy-Item -LiteralPath {ps(w.path('dll', 'ddraw.dll'))} -Destination "
           f"{ps(ntpath.join(work, 'ddraw.dll'))} -Force"])
    for lever in setup.get("levers", []):
        name, text = lever_file(lever)
        w.run([f"Set-Content -LiteralPath {ps(ntpath.join(work, name))} -Value {ps(text)} "
               f"-NoNewline -Encoding ASCII"])
    out = w.path("results", setup["name"] + ".jsonl")
    logdir = ntpath.join(work, "log")
    # what a recorder or anything else writes into log\ this run, and nothing older
    not_ours = "Where-Object { $_.Name -notlike 'tagpu*' -and $_.Name -ne 'startup-failure.txt' }"
    w.run([f"New-Item -ItemType Directory -Force -Path {ps(w.path('results'))} | Out-Null",
           f"Remove-Item -LiteralPath {ps(out)} -ErrorAction SilentlyContinue",
           f"Get-ChildItem -LiteralPath {ps(logdir)} -File -ErrorAction SilentlyContinue | "
           f"{not_ours} | Remove-Item -Force"])
    t0 = time.time()
    w.task(WIN_GAME_TASK, ntpath.join(work, "TotalA.exe"), "", work)
    w.task(WIN_WATCH_TASK, w.powershell32,
           f"-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File {w.path('watch.ps1')} "
           f"-Folder {work} -Seconds {watch} -Out {out}", w.root)
    events = []
    while time.time() - t0 < watch + 30:
        time.sleep(3)
        text = w.read_b64(out) or ""
        events = [json.loads(ln) for ln in text.splitlines() if ln.strip().startswith("{")]
        if any(e.get("done") for e in events):
            break
    result = w.task_result(WIN_GAME_TASK)
    alive = bool(w.game_procs()) and result == TASK_RUNNING
    stop_work_games(w, work)
    boxes, modules, hooks = [], [], None
    for e in events:
        if e.get("title") is not None and (e.get("class") == "#32770" or e.get("werfault")):
            boxes.append({"title": e["title"], "text": e.get("text", ""), "t": e.get("t")})
        if e.get("modules"):
            modules = e["modules"]
        if e.get("code"):
            hooks = win_hooks(e)
    # THE LOG IS READ ONLY ONCE NOTHING HOLDS IT. The DLL opens it without read sharing, and a
    # process that has been asked to stop lets go of its files some time after it stops being
    # listed -- so a read here can find the file in use even though stop_work_games waited. Stop
    # again and retry rather than lose the whole setup's evidence to it (seen 2026-09-27 on the
    # first setup of a run).
    tagpu = None
    for attempt in range(5):
        try:
            tagpu = w.read_b64(ntpath.join(work, "log", "tagpu.log"))
            break
        except taremote.RemoteError as e:
            if attempt == 4 or not re.search(r"IOException|another process|en cours d", str(e)):
                raise
            stop_work_games(w, work)
            time.sleep(3)
    o = {
        "setup": setup["name"], "platform": "windows",
        "watched": any(e.get("done") for e in events),
        "boxes": boxes, "alive_at_end": alive,
        "exit_code": None if alive else result,
        "impure_loaded": tagpu is not None,
        "packet_pub": packet_pub(tagpu or ""),
        "menu": None,
        "errorlog": w.read_b64(ntpath.join(work, "ErrorLog.txt")),
        "tdrawlog": w.read_b64(ntpath.join(work, "tdrawlog.txt")),
        "failure": w.read_b64(ntpath.join(work, "log", "startup-failure.txt")),
        "modules_known": any(e.get("modules") is not None for e in events),
        "folder_modules": modules, "hooks": hooks,
        "seconds": round(time.time() - t0, 1),
    }
    if o["exit_code"] is not None and o["exit_code"] >= 0xC0000000 and o["exit_code"] != TASK_RUNNING:
        o["errorlog"] = o["errorlog"] or f"exit code 0x{o['exit_code']:08X}"
    names = w.run([f"Get-ChildItem -LiteralPath {ps(logdir)} -File -ErrorAction SilentlyContinue | "
                   f"{not_ours} | ForEach-Object {{ $_.Name }}"])
    logs = {n.strip(): w.read_b64(ntpath.join(logdir, n.strip())) or "" for n in names if n.strip()}
    o["tadr_ran"] = tadr_evidence(o["tdrawlog"], logs) + hook_evidence(hooks)
    o["outcome"] = classify(o)
    o["gui"] = gui_health(tagpu or "")
    files = {"watch.jsonl": "\n".join(json.dumps(e) for e in events),
             **{f"log-{k}": v for k, v in logs.items()}}
    for k, fname in (("tdrawlog", "tdrawlog.txt"), ("failure", "startup-failure.txt"),
                     ("errorlog", "ErrorLog.txt")):
        if o[k]:
            files[fname] = o[k]
    if tagpu:
        files["tagpu.log"] = tagpu
    o["_files"] = files
    return o


def windows_cfg(args) -> dict:
    cfg = json.loads(WINDOWS_CFG.read_text()) if WINDOWS_CFG.exists() else {}
    for k in ("ssh", "key", "game"):
        if getattr(args, k):
            cfg[k] = getattr(args, k)
    missing = [k for k in ("ssh", "game") if not cfg.get(k)]
    if missing:
        die(f"say --{' --'.join(missing)} once; they are kept in {WINDOWS_CFG}")
    WINDOWS_CFG.parent.mkdir(parents=True, exist_ok=True)
    WINDOWS_CFG.write_text(json.dumps(cfg, indent=2) + "\n")
    return cfg


def cmd_windows(args):
    source = Path(args.dll).resolve()
    if not source.is_file():
        die(f"no DLL at {source}")
    dll = snapshot_dll(source)
    cfg = windows_cfg(args)
    setups = pick_setups(args.setups)
    ready = [s for s in setups if not any(fixture_problems(a["fixture"]) for a in s["add"])]
    if len(ready) < len(setups):
        print(f"skipped, fixtures not fetched: {', '.join(s['name'] for s in setups if s not in ready)}")
    if not ready:
        die("nothing to run")
    started = time.time()
    w = Win(cfg)
    try:
        running = w.game_procs()
        if running:
            die(f"TotalA.exe is already running on the machine ({'; '.join(running)}): it may be "
                f"the player's game, so nothing was started")
        game_exe = ntpath.join(cfg["game"], "TotalA.exe")
        if w.md5(game_exe) != RETAIL_MD5:
            die(f"{game_exe} is not the retail 3.1 exe: the base folder must be a 3.1 install")
        print(f"machine: {w.root}; copying the player's folder once into base (robocopy)")
        w.robocopy(cfg["game"], w.path("base"),
                   " /XF " + " ".join(WIN_EXCLUDE) + " /XD log Screenshots")
        needed = sorted({a["fixture"] for s in ready for a in s["add"]})
        for name in needed:
            for rel, want in FIX[name]["files"].items():
                target = w.path("fixtures", name, *Path(rel).parts)
                if w.md5(target) != want:
                    print(f"  upload {name}/{rel}")
                    w.upload(FIXTURES / name / rel, target)
        if w.md5(w.path("dll", "ddraw.dll")) != md5_file(dll):
            w.upload(dll, w.path("dll", "ddraw.dll"))
        w.upload(HERE / "win-watch.ps1", w.path("watch.ps1"))
        results = []
        for s in ready:
            print(f"  {s['name']} ...", end="", flush=True)
            o = run_windows_setup(w, s, args.watch)
            verdict(o, s, "windows")
            print(f" {o['outcome']} ({o['verdict']})")
            results.append(o)
    finally:
        try:
            if getattr(w, "work", None):
                stop_work_games(w, w.work)
        except Exception:  # noqa: BLE001 -- best effort; the report says what ran
            pass
        for t in (WIN_GAME_TASK, WIN_WATCH_TASK):
            try:
                w.task_remove(t)
            except Exception:  # noqa: BLE001 -- the report matters more than the tidy-up
                pass
        w.s.close()
    rc = report(results, "windows", dll, started, args.strict, source)
    shutil.rmtree(dll.parent, ignore_errors=True)
    return rc


# ------------------------------------------------------------------------- the decode, checked

# decode_run is what both platforms' verdicts rest on and what the DLL's own pass (pass 4 of
# tagpu_takeover.h) mirrors shape for shape, so it is worth being able to check without a game
# in front of you. One case a shape, built from what was actually measured, and one case each for
# the bytes that must NOT be taken for a hook -- both of which refused a launch on the Windows box
# before they were.
DECODE_CASES = [
    # name, stock bytes, the same bytes in memory, the module a VERDICT names (None: none)
    ("E9 rel32 into TADR",      b"\x90" * 16,                          "e9",       "tplayx.dll"),
    ("E8 displacement only",    b"\x90" * 4 + b"\xE8\x20\x00\x00\x00" + b"\x90" * 7,
                                                                       "e8keep",   "tplayx.dll"),
    ("the loader's thunk",      b"\x5E\x5D\xFF\x15\x00\xC1\x4F\x00" + b"\x90" * 8,
                                                                       "thunk",    "win32.dll"),
    ("push imm32; ret",         b"\x90" * 12,                          "pushret",  "tplayx.dll"),
    ("mov eax,imm32; jmp eax",  b"\x90" * 12,                          "moveax",   "tplayx.dll"),
    ("a bare address, held",    b"\x90" * 12,                          "abs",      None),
    ("an immediate (1500)",     b"\x90" * 12,                          "imm",      None),
    ("a displacement like one", b"\x90" * 16,                          "disp",     None),
    ("a mov that looks like it", b"\x90" * 12,                         "midmov",   None),
    ("FF15 pointer moved",      b"\x90" * 4 + b"\xFF\x15\x00\xC1\x4F\x00" + b"\x90" * 2,
                                                                       "ff15",     "tplayx.dll"),
    ("TADR later in the run",   b"\x90" * 16,                          "second",   "tplayx.dll"),
]


def cmd_selftest(args):
    """Every shape decode_run claims to decode, and every byte pattern it must not judge on.
    The module table is the one measured on the Windows box, `Dplayx.dll` at 0x00910000
    included, because where a DLL lands is what turns a coincidence into a false refusal."""
    va, tadr, mod = 0x00401000, 0x10001234, 0x6D4015A0
    mods = [(0x00910000, 0x0095F000, "Dplayx.dll", True),
            (0x10000000, 0x10050000, "tplayx.dll", True),
            (0x6D400000, 0x6D41E000, "win32.dll", False)]
    live = {
        "e9":      b"\x90" * 4 + b"\xE9" + struct.pack("<i", tadr - (va + 9)) + b"\x90" * 7,
        "e8keep":  b"\x90" * 4 + b"\xE8" + struct.pack("<i", tadr - (va + 9)) + b"\x90" * 7,
        "thunk":   b"\x5E\x5D\xE8" + struct.pack("<i", mod - (va + 7)) + b"\x90" * 9,
        "pushret": b"\x68" + struct.pack("<I", tadr) + b"\xC3" + b"\x90" * 6,
        "moveax":  b"\xB8" + struct.pack("<I", tadr) + b"\xFF\xE0" + b"\x90" * 5,
        "abs":     b"\x90" * 4 + struct.pack("<I", tadr) + b"\x90" * 4,
        "imm":     b"\x90" * 4 + struct.pack("<I", 1500) + b"\x90" * 4,
        # a jump of ours to a stub above the image: the displacement alone lands inside tplayx
        "disp":    b"\x90" * 4 + b"\xE9" + struct.pack("<I", 0x10001234) + b"\x90" * 7,
        # mov esi,[esi+0x92] -- its first four bytes read as 0x0092968B, inside tplayx here
        "midmov":  b"\x90" * 4 + b"\x8B\x96\x92\x00\x00\x00" + b"\x90" * 2,
        "ff15":    b"\x90" * 4 + b"\xFF\x15\x04\xC1\x4F\x00" + b"\x90" * 2,
        # two shapes in one changed run: the mod's own DLL first, TADR after it -- the TADR one
        # has to win, or a stock call into the mod's win32.dll hides a hook behind it
        "second":  b"\xE8" + struct.pack("<i", mod - (va + 5)) + b"\xE9"
                   + struct.pack("<i", tadr - (va + 10)) + b"\x90" * 6,
    }
    bad = 0
    for name, stock, key, want in DECODE_CASES:
        hooks = foreign_hooks(changed_runs(live[key], stock, va), mods,
                              lambda a: tadr if a == 0x004FC104 else 0)
        hits = [h for h in hooks if h["shaped"]]      # what a verdict is allowed to rest on
        got = hits[0]["module"] if hits else None
        where = f" 0x{hits[0]['site']:08X} -> 0x{hits[0]['target']:08X}" if hits else ""
        held = len(hooks) - len(hits)
        print(f"  {'ok ' if got == want else 'BAD'} {name:26} {got}{where}"
              f"{f'   ({held} held, not counted)' if held else ''}")
        bad += got != want
    print(f"decode: {len(DECODE_CASES) - bad} of {len(DECODE_CASES)} as intended")
    gbad = 0
    for name, text, exp, want in GUI_CASES:
        got = len(gui_misses(gui_health(text), exp))
        print(f"  {'ok ' if got == want else 'BAD'} {name:26} {got} miss(es)")
        gbad += got != want
    print(f"ui health: {len(GUI_CASES) - gbad} of {len(GUI_CASES)} as intended")
    hbad = 0
    for name, hud, exp, want in HUD_CASES:
        got = len(hud_misses(hud, exp))
        print(f"  {'ok ' if got == want else 'BAD'} {name:26} {got} miss(es)")
        hbad += got != want
    print(f"side panel: {len(HUD_CASES) - hbad} of {len(HUD_CASES)} as intended")
    return 1 if bad or gbad or hbad else 0


def _ask(why):
    return f"vk: gui: the twin store cannot follow the producer ({why}) - asking the producer\n"


_ACTIVE = {"outcome": "impure-active"}
_STRESS = {"outcome": "impure-active", "gui_stress": 2}
_FIRE = "gui: reseedstress: fresh start {} asked by the harness\n"
_IDX = "vk: gui: a restored sprite arrived while this lane holds no restored atlas - drawn indexed\n"
# (name, log, expectation, how many misses gui_misses must report)
GUI_CASES = [
    ("startup asks only", _ask("the presented surface has no twin here") * 2, _ACTIVE, 0),
    ("an ask past start-up", _ask("the presented twin has colour in the producer's record "
                                  "and none here"), _ACTIVE, 1),
    ("gave up", _ask("x") + "vk: " + GUI_GAVE_UP + " (x)\n", _ACTIVE, 2),
    ("stress clean", _FIRE.format(1) + _IDX + _FIRE.format(2), _STRESS, 0),
    ("stress, startup ask after", _FIRE.format(1) + _IDX + _FIRE.format(2) +
     _ask("the presented surface has no twin here"), _STRESS, 1),
    ("stress too few fires", _FIRE.format(1) + _IDX, _STRESS, 1),
    ("stress never indexed", _FIRE.format(1) + _FIRE.format(2), _STRESS, 1),
    ("stress budget spent", _FIRE.format(1) + _IDX + _FIRE.format(2) + GUI_SPENT + "\n", _STRESS, 1),
    ("not judged when inactive", _ask("x"), {"outcome": "impure-refused"}, 0),
    ("pixel drops, known work", "GUI pixels: gaf 60 copy-unseeded 323\n", _ACTIVE, 0),
    ("pixel drops, freed source", "GUI pixels: gaf 2\nGUI pixels: gaf 3 copy-freed 1\n", _ACTIVE, 1),
    ("pixel drops, none", "GUI pixels: none\n", _ACTIVE, 0),
]
# (name, the step's result, expectation, how many misses hud_misses must report); the numbers
# are the measured ones: the fix, v0.3, and a selection that did not take
HUD_CASES = [
    ("panel follows the engine", {"opened": 0.587, "stale": 0.0012}, _ACTIVE, 0),
    ("panel stays after deselect", {"opened": 0.587, "stale": 0.225}, _ACTIVE, 1),
    ("selection never opened", {"opened": 0.001, "stale": 0.0}, _ACTIVE, 1),
    ("captures missing", {"opened": None, "stale": None}, _ACTIVE, 1),
    ("step not run", None, _ACTIVE, 0),
    ("not judged when inactive", {"opened": 0.0, "stale": 0.5}, {"outcome": "impure-inactive"}, 0),
]


# ------------------------------------------------------------------------- cli

def main():
    ap = argparse.ArgumentParser(prog="tacompat.py", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch", help="download the fixtures, or import one fetched by hand")
    f.add_argument("--import", dest="imports", action="append", metavar="NAME=PATH")
    f.set_defaults(fn=cmd_fetch)
    sub.add_parser("list", help="setups, fixtures and what is missing").set_defaults(fn=cmd_list)
    default_dll = TREE / "tagpu" / "ddraw" / "ddraw.dll"
    wi = sub.add_parser("wine", help="run the setups on Wine, in parallel")
    wi.add_argument("setups", nargs="*")
    wi.add_argument("--dll", default=str(default_dll), help="the ddraw.dll under test (this tree's build)")
    wi.add_argument("--jobs", "-j", type=int, default=6)
    wi.add_argument("--watch", type=int, default=45, help="seconds each setup is watched")
    wi.add_argument("--screens", action="store_true", help="keep a picture of every run's display")
    wi.add_argument("--strict", action="store_true", help="a known gap fails the run too")
    wi.add_argument("--battle", type=int, default=60, metavar="SECONDS",
                    help="where the menu is reached, also fight a 200v200 skirmish this long "
                         "(0: start-up only)")
    wi.add_argument("--mp", type=int, default=30, metavar="SECONDS",
                    help="where Impure runs, also play a two-player network game this long "
                         "(0: none)")
    wi.add_argument("--mp-jobs", type=int, default=4, metavar="N",
                    help="network games at once, each on a DirectPlay port of its own")
    wi.set_defaults(fn=cmd_wine)
    wn = sub.add_parser("windows", help="run the setups on a Windows desktop, one at a time")
    wn.add_argument("setups", nargs="*")
    wn.add_argument("--dll", default=str(default_dll))
    wn.add_argument("--ssh", help="user@host (kept in the cache's windows.json)")
    wn.add_argument("--key", help="the SSH private key")
    wn.add_argument("--game", help="the player's retail 3.1 folder on the machine; only read")
    wn.add_argument("--watch", type=int, default=40)
    wn.add_argument("--strict", action="store_true", help="a known gap fails the run too")
    wn.set_defaults(fn=cmd_windows)
    sub.add_parser("clean", help="remove the Wine instances").set_defaults(fn=cmd_clean)
    sub.add_parser("selftest", help="check the hook decode against every shape it claims"
                   ).set_defaults(fn=cmd_selftest)
    args = ap.parse_args()
    sys.exit(args.fn(args))


if __name__ == "__main__":
    main()
