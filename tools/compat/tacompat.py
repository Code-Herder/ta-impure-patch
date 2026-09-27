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

`wine` gives each setup its own tacli instance (a private registry, see prepare_wine) and
its own virtual display, and runs them in parallel. `windows` copies the player's folder
once, then rebuilds one work folder per setup and starts the game on the machine's
desktop through a scheduled task. Both watch every window the game opens for the whole
run and read what each party logs. On Wine, a setup that reaches the main menu then
starts a skirmish and fights 200 against 200, because two patchers that both start can
still collide where the limits are used: `battle-crash` is a crash after the menu. Where
Impure runs, a second instance of the same folder then joins it in a two-player network game
over Windows' DirectPlay, one such game at a time (the port is the machine's). Every run, and
every peer, is read for TADR's code having run (tadr_evidence).
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
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(TREE / "tools"))
import taremote  # noqa: E402  PowerShell over SSH, one checked statement a line

TACLI = TREE / "tools" / "tacli"
DPINSTALL = TREE / "tools" / "dpinstall.sh"
DPLAY_SRC = Path(os.environ.get("TA_DIRECTPLAY_SRC", Path.home() / ".local/share/ta-directplay"))
DPLAY_OVERRIDES = "dplayx,dpmodemx,dpnet,dpnhpast,dpnhupnp,dpwsockx,dplaysvr.exe,dpnsvr.exe=n"
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
# Whether any of TADR's code ran, from what each part leaves behind. tdraw.dll writes
# tdrawlog.txt from its DllMain, and says there when it has written its engine patches (the
# old limit crack, the 2026 EngineLimits). The recorder -- tplayx.dll, or the 2006
# dplayx.dll -- writes "log\TA Demo Recorder Log -<date>.txt" in the game folder, and a
# "DLL.DirectPlay..." line in it only from inside one of its DirectPlay exports, which is
# where it starts: a log with no such line is a recorder that loaded and never ran.
TADR_INSTALLED = re.compile(r"Install Limit Crack|\[EngineLimits\] installed")
RECORDER_CALLED = re.compile(r"^\s*DLL\.DirectPlay", re.M)
# The two-player stage: small halves applied one per peer, each as that peer's own units.
MP_SCENARIOS = ("compat-mp-host", "compat-mp-join")
MP_PORT = 47624                         # DirectPlay's name server: one per machine
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
    if "tadr_ran" in exp and bool(ran) != exp["tadr_ran"]:
        miss.append("TADR ran: " + "; ".join(ran) if ran else "TADR did not run")
    if o.get("battle") is not None and not o["battle"].get("ok", False) and exp["outcome"] != "battle-crash":
        miss.append(f"the battle failed: {o['battle'].get('why', '?')}")
    if o.get("mp") is not None and not o["mp"].get("ok", False):
        miss.append(f"the network game failed: {o['mp'].get('why', '?')}")
    return miss


def tadr_evidence(tdrawlog, logs: dict, where="") -> list:
    """What says TADR's code ran: tdrawlog.txt at all, and a recorder log that an export
    call wrote into. `logs` maps a log folder file's name to its text."""
    ev = []
    if tdrawlog is not None:
        ev.append(f"{where}tdraw started (tdrawlog.txt" +
                  (", engine patches installed)" if TADR_INSTALLED.search(tdrawlog) else ")"))
    for name, text in sorted(logs.items()):
        if RECORDER_CALLED.search(text or ""):
            ev.append(f"{where}the recorder ran (log\\{name})")
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


def report(results, platform, dll, started, strict=False) -> int:
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
        {"platform": platform, "dll": str(dll), "dll_md5": md5_file(dll),
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
        net = "-" if mp is None else ("mp ok" if mp["ok"] else "mp FAILED")
        lines.append(f"  {mark[r['verdict']]} {r['setup']:22} {r['outcome']:18} {net:9}{box}")
        if r["verdict"] == "UNEXPECTED":
            for m in r.get("differs_today") or r.get("differs", []):
                lines.append(f"      {m}")
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


def free_display(taken: set) -> int:
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
    for _ in range(100):
        if Path(f"/tmp/.X11-unix/X{n}").exists():
            return p
        time.sleep(0.05)
    p.kill()
    die(f"Xvfb :{n} did not start")


def prepare_wine(setup, dll: Path, display: int, suffix="") -> dict:
    """A fresh tacli instance holding the setup's folder.

    Its registry is made PRIVATE: `tacli create` clones the prefix with hardlinks, so
    every instance's user.reg is one inode, and TA, TADR and the Patch Loader all write
    the registry. So the hives are copied into new files, but only after the wineserver
    that `create` started (for Wine's own settings) has exited: it rewrites the shared
    hive when it goes (tacli-shared-registry-inode). Creates therefore run one at a time."""
    name = PREFIX + re.sub(r"[^a-z0-9]+", "-", setup["name"].lower()).strip("-") + suffix
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
        (gamedir / lever).write_bytes(b"")
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


def wine_battle(inst, proc, display, gamedir, seen, boxes, t0, seconds) -> dict:
    """From the main menu into a skirmish and a 200-a-side fight, then watch it: two
    patchers that both started can still collide in play, where the limits are used.
    The menus are driven by tacli's gadget layer and the units placed by its scenario
    applier (scenarios/200v200.json), as in any tacli run."""
    name = inst["name"]
    for gadget in ("SINGLE", "Skirmish", "Start"):
        r = tacli("ui", name, "click", gadget, timeout=60)
        if r.returncode != 0:
            return {"ok": False, "why": f"ui click {gadget}: {(r.stderr or r.stdout).strip()[:200]}"}
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
    r = tacli("scenario", "apply", name, "200v200", timeout=180)
    if r.returncode != 0:
        return {"ok": False, "why": f"scenario apply: {(r.stderr or r.stdout).strip()[:200]}"}
    applied = (r.stdout.splitlines() or [""])[0]
    end = time.time() + seconds
    while time.time() < end:
        time.sleep(2)
        new_boxes(display, seen, boxes, t0)
        if proc.poll() is not None:
            return {"ok": False, "why": f"the game exited during the battle ({applied})"}
        if boxes or (gamedir / "ErrorLog.txt").exists():
            return {"ok": False, "why": f"a box or a crash report during the battle ({applied})"}
    return {"ok": True, "why": applied}


def run_wine(setup, dll, watch, display, keep_screens, battle=0) -> dict:
    inst = setup["_inst"]
    gamedir, prefix = inst["gamedir"], inst["prefix"]
    env = dict(os.environ, WINEPREFIX=str(prefix), DISPLAY=f":{display}",
               WINEDLLOVERRIDES=f"ddraw=n,b;{DPLAY_OVERRIDES}", WINEDEBUG="+loaddll")
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
        fight = None
        if battle and menu and not boxes and proc.poll() is None:
            fight = wine_battle(inst, proc, display, gamedir, seen, boxes, t0, battle)
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
        xv.kill()
        xv.wait()
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
        "battle": fight,
        "seconds": round(time.time() - t0, 1),
    }
    logs = other_logs(gamedir / "log", t0)
    o["tadr_ran"] = tadr_evidence(o["tdrawlog"], logs)
    o["outcome"] = classify(o)
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
    checks a mechanism, not a game."""
    return setup["goal"]["outcome"] == "impure-active" and not setup.get("levers")


def dplay_holders() -> list:
    """(pid, WINEPREFIX) of every process listening on DirectPlay's port."""
    r = subprocess.run(["ss", "-lunpH", f"sport = :{MP_PORT}"], capture_output=True, text=True,
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


def free_dplay_port(wait=300) -> "str | None":
    """Make the port free for our host, or say who holds it. A holder in one of this tool's
    instances is ours and stale, and its wineserver is ended; any other is someone else's
    game, which is waited for and never touched (parallel-mp-runs-share-dplay-port)."""
    deadline = time.time() + wait
    while True:
        holders = dplay_holders()
        if not holders:
            return None
        ours = [pf for _, pf in holders if Path(pf).parent.name.startswith(PREFIX)]
        for pf in ours:
            subprocess.run(["wineserver", "-k"], env=dict(os.environ, WINEPREFIX=pf),
                           capture_output=True, timeout=30)
        if not ours and time.time() > deadline:
            return ", ".join(f"pid {pid} ({pf or 'no WINEPREFIX'})" for pid, pf in holders)
        time.sleep(2 if ours else 5)


def start_wine(inst, display):
    """TotalA.exe in the instance's game folder on its own virtual display."""
    env = dict(os.environ, WINEPREFIX=str(inst["prefix"]), DISPLAY=f":{display}",
               WINEDLLOVERRIDES=f"ddraw=n,b;{DPLAY_OVERRIDES}", WINEDEBUG="+loaddll")
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
    g["xv"].kill()
    g["xv"].wait()
    g["log"].close()
    os.unlink(g["log"].name)


def lobby_ui(inst, *argv, timeout=40) -> str:
    r = tacli("ui", inst, *argv, "--timeout", str(timeout), timeout=timeout + 30)
    if r.returncode != 0:
        raise Lobby(f"{inst}: ui {' '.join(argv)}: {(r.stderr or r.stdout).strip()[:200]}")
    return r.stdout


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


def wine_lobby(host, join):
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
    for _ in range(6):
        if re.search(r"^grayed\s+0\s*$", lobby_ui(join, "show", "JOINGAME"), re.M):
            break
        lobby_ui(join, "click", "UPDATE")
        time.sleep(5)
    lobby_ui(join, "click", "JOINGAME", timeout=30)
    lobby_ui(join, "wait", "--gui", "LOUNGE2", timeout=30)
    lobby_ui(join, "click", "READY0", timeout=20)     # each client lists itself as row 0
    lobby_ui(host, "click", "READY0", timeout=20)
    lobby_ui(host, "click", "START", timeout=30)
    for inst in (host, join):
        r = tacli("wait", inst, "alive=[1-9]", "--timeout", "150", timeout=200)
        if r.returncode != 0:
            raise Lobby(f"{inst}: the game never came alive: {(r.stderr or r.stdout).strip()[:200]}")


def run_wine_mp(setup, dll, seconds, displays) -> dict:
    """Two players, the setup's folder on each, hosted and joined through the game's own
    battle room over Windows' DirectPlay, a small fight between them, then watched. Every
    peer's folder is read for TADR's evidence. One game at a time: see free_dplay_port."""
    create_display, host_display, join_display = displays
    xv = start_xvfb(create_display)
    try:
        join = prepare_wine(setup, dll, create_display, suffix="-j")
    finally:
        xv.kill()
        xv.wait()
    held = free_dplay_port()
    if held:
        return {"ok": False, "why": f"could not run: DirectPlay's port {MP_PORT} is held by {held}",
                "evidence": []}
    peers = {"host": setup["_inst"], "join": join}
    t0 = time.time()
    games = {}
    boxes, seen, why = [], set(), None
    shots = {}
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
            wine_lobby(peers["host"]["name"], peers["join"]["name"])
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
        except Lobby as e:
            why = str(e)
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
        out["evidence"] += tadr_evidence(tdrawlog, logs, where=f"network game, {role}: ")
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


def cmd_wine(args):
    dll = Path(args.dll).resolve()
    if not dll.is_file():
        die(f"no DLL at {dll} (build it: make -C tagpu/ddraw -j$(nproc))")
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
        xv.kill()
        xv.wait()
    displays = {s["name"]: free_display(taken) for s in ready}
    print(f"running {len(ready)} setups, {args.jobs} at a time, {args.watch} s each"
          + (f"; a two-player game of {args.mp} s for each that starts" if args.mp else ""))
    results, mp_futs = [], {}
    # One network game at a time (DirectPlay's port is the machine's), each queued the
    # moment its setup's single-player run shows Impure running, beside the other runs.
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool, \
            concurrent.futures.ThreadPoolExecutor(max_workers=1) as mp_pool:
        futs = {pool.submit(run_wine, s, dll, args.watch, displays[s["name"]], args.screens,
                            args.battle): s
                for s in ready}
        for f in concurrent.futures.as_completed(futs):
            s = futs[f]
            o = f.result()
            verdict(o, s, "wine")
            print(f"  {s['name']}: {o['outcome']} ({o['verdict']})")
            results.append(o)
            if args.mp and mp_eligible(s) and o["outcome"] == "impure-active" and o.get("menu"):
                mp_futs[s["name"]] = (s, o, mp_pool.submit(
                    run_wine_mp, s, dll, args.mp, [free_display(taken) for _ in range(3)]))
        for name, (s, o, f) in mp_futs.items():
            m = f.result()
            o["_files"].update(m.pop("_files", {}))
            o["_copies"].update(m.pop("_copies", {}))
            o["tadr_ran"] = o.get("tadr_ran", []) + m["evidence"]
            o["mp"] = m
            verdict(o, s, "wine")
            print(f"  {name}: network game {'ok' if m['ok'] else 'FAILED: ' + m['why']} ({o['verdict']})")
    results.sort(key=lambda r: [s["name"] for s in SETUPS].index(r["setup"]))
    return report(results, "wine", dll, started, args.strict)


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


def run_windows_setup(w: Win, setup, watch) -> dict:
    work, base = w.path("work"), w.path("base")
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
        w.run([f"New-Item -ItemType File -Force -Path {ps(ntpath.join(work, lever))} | Out-Null"])
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
    boxes, modules = [], []
    for e in events:
        if e.get("title") is not None and (e.get("class") == "#32770" or e.get("werfault")):
            boxes.append({"title": e["title"], "text": e.get("text", ""), "t": e.get("t")})
        if e.get("modules"):
            modules = e["modules"]
    tagpu = w.read_b64(ntpath.join(work, "log", "tagpu.log"))
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
        "folder_modules": modules,
        "seconds": round(time.time() - t0, 1),
    }
    if o["exit_code"] is not None and o["exit_code"] >= 0xC0000000 and o["exit_code"] != TASK_RUNNING:
        o["errorlog"] = o["errorlog"] or f"exit code 0x{o['exit_code']:08X}"
    names = w.run([f"Get-ChildItem -LiteralPath {ps(logdir)} -File -ErrorAction SilentlyContinue | "
                   f"{not_ours} | ForEach-Object {{ $_.Name }}"])
    logs = {n.strip(): w.read_b64(ntpath.join(logdir, n.strip())) or "" for n in names if n.strip()}
    o["tadr_ran"] = tadr_evidence(o["tdrawlog"], logs)
    o["outcome"] = classify(o)
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
    dll = Path(args.dll).resolve()
    if not dll.is_file():
        die(f"no DLL at {dll}")
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
            stop_work_games(w, w.path("work"))
        except Exception:  # noqa: BLE001 -- best effort; the report says what ran
            pass
        for t in (WIN_GAME_TASK, WIN_WATCH_TASK):
            try:
                w.task_remove(t)
            except Exception:  # noqa: BLE001 -- the report matters more than the tidy-up
                pass
        w.s.close()
    return report(results, "windows", dll, started, args.strict)


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
                    help="where Impure runs, also play a two-player network game this long, "
                         "one game at a time (0: none)")
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
    args = ap.parse_args()
    sys.exit(args.fn(args))


if __name__ == "__main__":
    main()
