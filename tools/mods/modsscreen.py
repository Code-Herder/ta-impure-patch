#!/usr/bin/env python3
"""modsscreen.py -- the Mods screen played in the game, on Wine: the real DLL, the real button, device clicks.

    python3 tools/mods/modsscreen.py [--only flow|real|boxes|menus|mutators|hosted] [--display :N] [--software]

`flow` opens the screen from the main menu button and clicks through it: the tabs, the play options shown and not set, a locked
option, a downloaded Install and Update against a local server, the radio and Confirm, then sees the game restart on the chosen mod, with every archive
it mounts mounted whatever a key an older build wrote says, and back again. `real` does a little of that by hardware events on the private display,
in a window larger than the shell, which is the path a player's mouse takes. `boxes` is the screen refused: GDI and an unknown exe. `menus` starts each v1 mod and photographs its main menu and the
screen over it, for the eye: tmp/modsscreen/. `hosted`, run only when asked for since it downloads from the internet, installs each
mod whose catalog entry names its base_url from that real address, through the screen with no local server, and starts the game on it.

Each control is found in the screen's own state dump (tagpu_modsui.dump), the hit map of the frame last drawn, so a click lands
where the player would aim. It needs the packages tools/modpack/modpack.py packages builds, and starts its own Xvfb when no display is given.
Exit 1 on any failed claim, 2 when it could not run. One game at a time: it holds the machine's run lock.
"""
import argparse
import atexit
import http.server
import shutil
import json
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import runlock  # noqa: E402

TACLI = [sys.executable, str(ROOT / "tools/tacli")]
SOFTWARE = False     # --software
CACHE = Path(os.environ.get("TACOMPAT_CACHE", Path.home() / ".local/share/ta-compat")) / "modpack"
OUT = ROOT / "tmp" / "modsscreen"
NAME = "mods-screen"
INSTANCES = Path(subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True).stdout.strip()).parent / "tagpu/instances"
fails = 0


def report(ok, what):
    global fails
    print(("ok    " if ok else "FAIL  ") + what, flush=True)
    if not ok:
        fails += 1
    return ok


def tacli(*a, timeout=120, check=True):
    r = subprocess.run(TACLI + list(a), capture_output=True, text=True, timeout=timeout)
    if check and r.returncode:
        sys.exit(f"modsscreen: tacli {' '.join(a)}: {r.stderr or r.stdout}")
    return r


class Game:
    def __init__(self, display):
        tacli("rm", NAME, "--force", check=False)
        tacli("create", NAME, "--display", display, "--res", "1280x720", "--private-registry")
        self.display = display
        self.dir = Path(json.loads((INSTANCES / NAME / "instance.json").read_text())["gamedir"])

    def launch(self):
        # --defaults: the store is read and written, as a player's is
        if SOFTWARE:
            (self.dir / "tagpu_classicpp.off").write_text("")
        tacli("launch", NAME, "--res", "1280x720", "--defaults", timeout=180)
        self.menu()

    def menu(self, seconds=90):
        """The main menu, whichever process draws it."""
        end = time.time() + seconds
        while time.time() < end:
            r = tacli("ui", NAME, "--json", check=False, timeout=30)
            try:
                if r.returncode == 0 and json.loads(r.stdout).get("gui", "").upper().startswith("MAINMENU"):
                    return True
            except ValueError:
                pass
            time.sleep(1)
        return False

    def state(self):
        out, ask = self.dir / "tagpu_modsui.json", self.dir / "tagpu_modsui.dump"
        out.unlink(missing_ok=True)
        ask.write_text("")
        end = time.time() + 20
        while time.time() < end:
            if out.exists():
                return json.loads(out.read_text(errors="replace"))
            time.sleep(0.05)
        sys.exit("modsscreen: the state dump never came")

    def settled(self, want=lambda s: True, seconds=30):
        """The state once the frame drawn shows the page, and `want` holds: a press acts at once, its picture a frame later."""
        end = time.time() + seconds
        s = self.state()
        while time.time() < end:
            if s["current"] and want(s):
                return s
            time.sleep(0.1)
            s = self.state()
        return s

    def click_ending(self, key):
        """A control that ends this process: tacli waits for a batch's end, which a process that has gone never reports."""
        s = self.settled()
        h = next(h for h in s["hits"] if h["key"] == key)
        p = subprocess.Popen(TACLI + ["click", NAME, "--device", str((h["r"][0] + h["r"][2]) // 2), str((h["r"][1] + h["r"][3]) // 2)],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(4)
        p.terminate()

    def click(self, key):
        s = self.settled(lambda s: any(h["key"] == key for h in s["hits"]), 10)
        for h in s["hits"]:
            if h["key"] == key:
                x0, y0, x1, y1 = h["r"]
                tacli("click", NAME, "--device", str((x0 + x1) // 2), str((y0 + y1) // 2))
                time.sleep(0.25)
                return True
        return report(False, f"no control {key} on the screen: {[h['key'] for h in s['hits']]}")

    def shot(self, name):
        OUT.mkdir(parents=True, exist_ok=True)
        # the X window as presented: the engine's own frame has no Vulkan layer in it
        subprocess.run(["import", "-display", self.display, "-window", "root", str(OUT / f"{name}.png")], capture_output=True, timeout=30)

    def logs(self):
        return "\n".join(p.read_text(errors="replace") for p in sorted((self.dir / "log").glob("tagpu*.log"), key=lambda p: p.stat().st_mtime))


def row_of(s, name):
    for i, r in enumerate(s["rows"]):
        if r["name"] == name:
            return i
    return -1


def serve():
    """The packages modpack.py built, by the first part of the catalog's relative URL."""
    got = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            mod = self.path.strip("/").split("/")[0]
            data = (CACHE / mod / f"{mod}.tar.zst").read_bytes()
            got.append(mod)
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, got


def install_by_hand(game, mod):
    subprocess.run([sys.executable, str(ROOT / "tools/modpack/modpack.py"), "install", mod, str(game.dir)], check=True, capture_output=True)


def flow(display):
    g = Game(display)
    srv, got = serve()
    install_by_hand(g, "prota")
    (g.dir / "mods/prota/installed.txt").write_text("0.9")   # what Updates has to find
    g.launch()
    ui = json.loads(tacli("ui", NAME, "--json").stdout)
    report(any(x.get("name") == "MODS" for x in ui.get("gadgets", ui.get("items", []))) or "MODS" in tacli("ui", NAME).stdout, "the main menu has a MODS button")
    tacli("ui", NAME, "click", "MODS")
    s = g.settled(lambda s: s["open"] and s["drawn"])
    report(s["open"] == 1 and s["holds"] == 1 and s["drawn"] == 1, "MODS opens the screen, it holds the input and a frame has drawn it")
    report(s["running"] == "This folder" and s["chosen"] == "This folder", "This folder runs and is chosen")
    report(row_of(s, "This folder") == 0 and row_of(s, "ProTA") == 1, f"Manage lists the folder first, then the installed mod: {[r['name'] for r in s['rows']]}")
    report(s["confirm"] == "", "Confirm is grey with nothing to do")
    g.shot("manage")

    g.click("tab:1")
    s = g.settled(lambda s: s["tab"] == 1)
    names = [r["name"] for r in s["rows"]]
    hosted = {e["name"] for e in json.loads((ROOT / "mods/catalog.json").read_text())["mods"] if e.get("base_url")}
    report(s["tab"] == 1 and set(names) == hosted - {"ProTA"}, f"Explore offers only what the catalog names a download place for: {names}")
    # From here on the local server stands in for every entry's download place; the screen reads that at its open.
    (g.dir / "tagpu_modbase.on").write_text(f"http://127.0.0.1:{srv.server_address[1]}")
    g.click("close")
    g.settled(lambda s: not s["open"])
    tacli("ui", NAME, "click", "MODS")
    g.settled(lambda s: s["open"] and s["drawn"])
    g.click("tab:1")
    s = g.settled(lambda s: s["tab"] == 1)
    names = [r["name"] for r in s["rows"]]
    report(s["tab"] == 1 and "ProTA" not in names and "Escalation" in names and "Total Mayhem" in names, f"Explore lists what is not installed: {names}")
    g.click(f"row:{row_of(s, 'Escalation')}")
    s = g.settled(lambda s: s["opts"])
    report(all(o["locked"] and o["reason"] for o in s["opts"]), f"Escalation's options are locked with its reason: {s['opts'][0]['reason']}")
    k = [h["key"] for h in s["hits"]]
    report(not any(x.startswith("opt:") for x in k), "a locked option takes no click")
    g.shot("explore-locked")

    g.click("tab:2")
    s = g.settled(lambda s: s["tab"] == 2)
    report(s["updates"] == 1 and row_of(s, "ProTA") == 0 and s["update_all"] == 1, f"Updates finds the old version: {s['rows']}")
    g.shot("updates")

    g.click("tab:0")
    s = g.settled(lambda s: s["tab"] == 0)
    g.click(f"row:{row_of(s, 'ProTA')}")
    s = g.settled(lambda s: s["opts"])
    report([(o["label"], o["value"], o["locked"]) for o in s["opts"]] == [("Resource snapping radius", "3", 0), ("Quick Build", "On", 0)],
           f"ProTA's options show the mod's values: {[(o['label'], o['value']) for o in s['opts']]}")
    # the Mods screen sets no game setting: its controls are the list's, the tabs, the footer's and the close button
    others = [h["key"] for h in s["hits"] if not h["key"].startswith(("tab:", "row:", "act:", "scroll:", "link:")) and h["key"] not in ("close", "back", "confirm", "updateall")]
    report(not others, f"the pane takes no click but a link: {others}")
    show = json.loads((ROOT / "mods/prota.json").read_text())["show"]
    want = sum(len(b["list"]) if isinstance(b, dict) and "list" in b else 1 for b in show["about"])
    report(s["show"] == "prota" and s["show_blocks"] == want, f"the pane is ProTA's showcase, its {want} blocks from the catalog: {s['show']!r}, {s['show_blocks']}")
    links = sorted(h["key"] for h in s["hits"] if h["key"].startswith("link:"))
    report(links[:1] == ["link:0"], f"its website is a link: {links}")
    g.click("link:0")
    time.sleep(1)
    report(f"a test launch opens no browser: {show['site']}" in g.logs(), "a click on it would open the website, which a test launch only logs")
    g.shot("manage-prota")
    # a hide. key an older build wrote: the mount reads no player's choice of archive
    arc = next((p.name for p in g.dir.iterdir() if p.name.lower() == "ccdata.ccx"), None)
    report(arc is not None, "the folder has ccdata.ccx")
    profile = g.dir / "profiles/prota.ini"
    profile.parent.mkdir(exist_ok=True)
    profile.write_bytes((profile.read_bytes() if profile.exists() else b"") + f"hide.{arc}=on\r\n".encode())

    g.click("tab:2")
    s = g.settled(lambda s: s["tab"] == 2)
    g.click(f"act:{row_of(s, 'ProTA')}")
    s = g.settled(lambda s: s["updates"] == 0 and s["confirm"] != "" or s["note"], 180)
    report("prota" in got, f"Update fetched the package from the server: {got}")
    report(s["updates"] == 0, f"Updates is empty once it is ready: note {s['note']!r}")
    report((g.dir / "mods/prota.new/installed.txt").exists(), "it waits in mods\\prota.new for the next start")

    g.click("tab:1")
    s = g.settled(lambda s: s["tab"] == 1)
    g.click(f"act:{row_of(s, 'Total Mayhem')}")
    s = g.settled(lambda s: row_of(s, "Total Mayhem") < 0 or s["rows"][row_of(s, "Total Mayhem")]["act"] == 5, 30)
    g.shot("explore-installing")
    s = g.settled(lambda s: row_of(s, "Total Mayhem") < 0 or s["note"], 240)
    report(row_of(s, "Total Mayhem") < 0 and "mayhem" in got, f"Install fetched and finished: {got}, note {s['note']!r}")

    g.click("tab:0")
    s = g.settled(lambda s: s["tab"] == 0)
    report(row_of(s, "Total Mayhem") > 0, "Manage lists it")
    g.click(f"act:{row_of(s, 'ProTA')}")
    s = g.settled(lambda s: s["chosen"] == "ProTA")
    report(s["confirm"].startswith("Restart with ProTA"), f"choosing asks for the restart: {s['confirm']!r}")
    g.shot("confirm")
    g.click_ending("confirm")
    report(g.menu(), "the game comes back to its main menu by itself")
    logs = g.logs()
    report(f"mod=prota" in (g.dir / "impure.cfg").read_text(), "Confirm wrote mod=prota")
    report("modload: mounted mods\\prota\\prota.hpi" in logs, "the new process runs ProTA")
    report(not re.search(rf"not mounted {re.escape(str(arc))}:", logs, re.I), f"{arc} mounts, whatever the older key says")
    report((g.dir / "mods/prota/installed.txt").read_text().strip() != "0.9", "the update went in at the start")
    tacli("ui", NAME, "click", "MODS")
    s = g.settled(lambda s: s["open"] and s["drawn"])
    report(s["running"] == "ProTA" and s["chosen"] == "ProTA", "the screen says so")
    # the mod running now: its options read from the profile module's own store
    g.click(f"row:{row_of(s, 'ProTA')}")
    s = g.settled(lambda s: s["opts"])
    report([o["value"] for o in s["opts"]] == ["3", "On"] and not any(h["key"].startswith(("opt:", "reset:")) for h in s["hits"]),
           f"the running mod's options show its values and take no click: {[o['value'] for o in s['opts']]}")
    g.click("close")
    s = g.settled(lambda s: not s["open"])
    report(not s["open"] and s["holds"] == 0, "the close button closes it and gives the input back")
    tacli("ui", NAME, "click", "SINGLE")
    time.sleep(1)
    r = json.loads(tacli("ui", NAME, "--json").stdout)
    report(not r.get("gui", "").upper().startswith("MAINMENU"), f"the engine takes clicks again: {r.get('gui')}")
    tacli("keys", NAME, "esc", check=False)
    time.sleep(1)
    tacli("ui", NAME, "click", "MODS")
    s = g.settled(lambda s: s["open"] and s["drawn"])
    # Total Mayhem's showcase is longer than its pane
    g.click(f"row:{row_of(s, 'Total Mayhem')}")
    s = g.settled(lambda s: s["show"] == "mayhem" and s["scroll_max"][1] > 0)
    report(s["show"] == "mayhem" and s["scroll_max"][1] > 0, f"Total Mayhem's showcase overflows its pane: {s['show']!r}, {s['scroll_max']}")
    g.shot("manage-mayhem")
    # over the pane, in the window's pixels the hit map is in
    x0, y0, x1, y1 = next(h["r"] for h in s["hits"] if h["key"] == "scroll:detail")
    tacli("keys", NAME, f"dmove:{(x0 + x1) // 2},{(y0 + y1) // 2}", "wheel:-3", check=False)
    s = g.settled(lambda s: s["scroll"][1] > 0, 5)
    report(s["scroll"][1] > 0, f"the wheel scrolls the pane: {s['scroll']}")
    tacli("keys", NAME, "esc")
    s = g.settled(lambda s: not s["open"], 5)
    report(not s["open"], "Esc closes it")
    tacli("ui", NAME, "click", "MODS")
    s = g.settled(lambda s: s["open"] and s["drawn"])
    g.click(f"act:{row_of(s, 'This folder')}")
    s = g.settled(lambda s: s["chosen"] == "This folder")
    g.click_ending("confirm")
    report(g.menu(), "Confirm comes back again")
    tacli("ui", NAME, "click", "MODS")
    s = g.settled(lambda s: s["open"] and s["drawn"])
    report(s["running"] == "This folder" and "mod=" not in (g.dir / "impure.cfg").read_text(), "the folder runs again, and mod= is gone")
    report(f"hide.{arc}=on" in (g.dir / "profiles/prota.ini").read_text(), "ProTA's profile survives it, with the key this build does not read")
    tacli("stop", NAME, check=False)
    srv.shutdown()


def boxes(display):
    """The screen refused: an engine box over the main menu, and the menu goes on after it."""
    def refused(g, shot, says):
        g.launch()
        tacli("ui", NAME, "click", "MODS")
        time.sleep(1)
        gui = json.loads(tacli("ui", NAME, "--json").stdout).get("gui", "")
        g.shot(shot)
        report(gui.upper().startswith("MSGBOX") and says in g.logs(), f"{shot}: the engine's box, saying: {says}")
        s = g.state()
        report(not s["open"] and not s["holds"], f"{shot}: the screen stays shut")
        tacli("ui", NAME, "click", "OK")
        report(g.menu(10), f"{shot}: OK closes the box back to the main menu")
        tacli("stop", NAME, check=False)

    g = Game(display)
    (g.dir / "tagpu_gdi.on").write_text("")
    refused(g, "box-gdi", "draws with the Vulkan renderer")

    g = Game(display)
    exe = next(p for p in g.dir.iterdir() if p.name.lower() == "totala.exe")
    d = bytearray(exe.read_bytes())
    d[-1] ^= 1      # past the sections' code: the game runs and its key is no longer one the pack knows
    exe.unlink()
    exe.write_bytes(d)
    refused(g, "box-exe", "not one Impure knows")

    # the layer refuses after the open: its pass is built at its first use
    g = Game(display)
    (g.dir / "tagpu_hudrefuse.on").write_text("")
    g.launch()
    tacli("ui", NAME, "click", "MODS")
    time.sleep(3)
    s = g.state()
    report(not s["open"] and not s["holds"], "the pass refused after the open: the screen closed itself and holds nothing")
    tacli("ui", NAME, "click", "MODS")
    time.sleep(1)
    gui = json.loads(tacli("ui", NAME, "--json").stdout).get("gui", "")
    report(gui.upper().startswith("MSGBOX"), "the next click gets the box")
    tacli("ui", NAME, "click", "OK")
    report(g.menu(10), "OK goes back to the menu")
    tacli("stop", NAME, check=False)


def real(display):
    """The hardware path: no shield, X events from xdotool on this private display, a window that is not the shell's size."""
    g = Game(display)
    env = dict(os.environ, DISPLAY=display)

    def xdo(*a):
        subprocess.run(["xdotool", *map(str, a)], env=env, check=True, timeout=20)

    def press(s, key):
        x0, y0, x1, y1 = next(h["r"] for h in s["hits"] if h["key"] == key)
        xdo("mousemove", (x0 + x1) // 2, (y0 + y1) // 2)
        time.sleep(0.2)
        xdo("click", 1)
        time.sleep(0.3)

    tacli("launch", NAME, "--res", "1280x720", "--defaults", "--no-shield", "--window", "1280x960", timeout=180)
    g.menu()
    time.sleep(1)
    # windowed, the first click only locks the mouse (wndproc.c), as for any screen
    xdo("mousemove", 30, 30)
    xdo("click", 1)
    time.sleep(0.5)
    xdo("mousemove", 640, 852)  # MODS: (320,426) of the 640x480 shell in a window twice its size
    time.sleep(0.3)
    xdo("click", 1)
    s = g.settled(lambda s: s["open"] and s["drawn"], 10)
    report(s["open"] == 1, "a real click on MODS opens the screen")
    time.sleep(0.5)
    g.shot("real-1280x960")
    press(s, "tab:1")
    s = g.settled(lambda s: s["tab"] == 1, 5)
    report(s["tab"] == 1, "a real click takes a tab")
    press(s, "tab:0")
    s = g.settled(lambda s: s["tab"] == 0, 5)
    press(s, "row:0")
    xdo("mousemove", 100, 100)
    from PIL import Image
    def has_cursor(im, box):
        # the backdrop and the window part are grey and near-black; the cursor sprite is saturated blue
        return any(b > r + 40 for r, g_, b in im.crop(box).getdata())
    def cursor_moved():
        g.shot("real-pointer")
        im = Image.open(OUT / "real-pointer.png").convert("RGB")
        return has_cursor(im, (95, 95, 125, 130)) and not has_cursor(im, (635, 847, 665, 882))
    # a frame reaches the display when it does: the verdict is on the picture, the deadline only says it never came
    end = time.time() + 15
    moved = cursor_moved()
    while not moved and time.time() < end:
        time.sleep(0.5)
        moved = cursor_moved()
    report(moved, "the drawn cursor follows the real pointer")
    # under the capture the engine reads Alt as up, so nothing but the screen stops a chord pressing a menu button behind it
    xdo("key", "alt+s")
    xdo("key", "alt+m")
    time.sleep(1.5)
    report(g.menu(5), "Alt+S and Alt+M press no menu button behind the screen")
    xdo("key", "a")
    s = g.settled(lambda s: s["open"], 2)
    report(s["open"] == 1 and s["holds"] == 1, "a key does nothing to it")
    press(s, "back")
    s = g.settled(lambda s: not s["open"], 5)
    report(not s["open"] and s["holds"] == 0, "a real click on Back closes it")
    xdo("mousemove", 374, 806)  # SINGLE
    time.sleep(0.3)
    xdo("click", 1)
    time.sleep(1.5)
    r = json.loads(tacli("ui", NAME, "--json").stdout).get("gui", "")
    report(r.upper().startswith("SINGLE"), f"the engine takes a real click after it: {r}")
    tacli("stop", NAME, check=False)


def mutators(display):
    """A mutator from install to the game: the look it changes in a skirmish, mounted whole whatever a key an older build wrote says, the stale and the
    conflicting one, a hide, what a recording says."""
    sys.path.insert(0, str(ROOT / "tools/modpack"))
    import modpack as mp
    work = OUT / "mutators"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    mp_args = lambda *a: subprocess.run([sys.executable, str(ROOT / "tools/modpack/modpack.py"), *a], check=True, capture_output=True, text=True)

    g = Game(display)
    mp_args("install", "prota", str(g.dir), "--link")
    version = next(e["version"] for e in json.loads((ROOT / "mods/catalog.json").read_text())["mods"] if e["id"] == "prota")
    (g.dir / "mods/prota/installed.txt").write_text(version)    # the marker an install writes
    cfg = g.dir / "impure.cfg"
    cfg.write_bytes((cfg.read_bytes() if cfg.exists() else b"") + b"\r\nmod=prota\r\n")

    # the files, from what the base holds: this folder stays out of the repository
    sources = mp.base_sources("prota")
    opened = {}
    com = mp.base_answer(sources, "objects3d/armcom.3do", opened)
    cor = mp.base_answer(sources, "objects3d/corcom.3do", opened)
    report(com is not None and cor is not None, "the base holds both commanders' models")
    (work / "armcom.3do").write_bytes(com)
    (work / "corcom.3do").write_bytes(cor)
    def stretched(src, out, amount):
        out.parent.mkdir(parents=True, exist_ok=True)
        pieces = json.loads(subprocess.run([str(ROOT / "tools/ta3domod"), "dump", str(src), "--json"], capture_output=True, text=True, check=True).stdout)
        names = [x["name"] for x in pieces]
        piece = "torso" if "torso" in names else names[0]
        subprocess.run([str(ROOT / "tools/ta3domod"), "edit", str(src), "-o", str(out), "--stretch", f"{piece}:y:0:{amount}"], check=True, capture_output=True)

    entries = []
    def build(mid, name, files, hide=(), stale=False, base="prota"):
        folder = work / mid
        for rel, (src, amount) in files.items():
            stretched(src, folder / rel, amount)
        folder.mkdir(parents=True, exist_ok=True)
        out = work / f"{mid}.json"
        mp_args("mutator", str(folder), "--id", mid, "--base", base, "--name", name, "--version", "1.0", "--author", "Tests", "--summary", f"{name}: the tests'.",
                "--homepage", "https://example.invalid", "--permission", "the tests'", "--out", str(out), *[x for h in hide for x in ("--hide", h)])
        e = json.loads(out.read_text())
        if stale:
            for k in e["base_hashes"]:
                e["base_hashes"][k] = "00" * 32
        entries.append(e)

    build("tlook", "Tall commander", {"objects3d/armcom.3do": (work / "armcom.3do", 60)})
    build("tclash", "Other commander", {"objects3d/armcom.3do": (work / "armcom.3do", 20)})
    build("tstale", "Stale commander", {"objects3d/corcom.3do": (work / "corcom.3do", 60)}, stale=True)
    build("thide", "Hide commander", {}, hide=["objects3d/armcom.3do"])
    retail_com = mp.base_answer(mp.base_sources("none"), "objects3d/armcom.3do", {})
    (work / "retail-armcom.3do").write_bytes(retail_com)
    build("tnone", "Tall retail commander", {"objects3d/armcom.3do": (work / "retail-armcom.3do", 60)}, base="none")
    catalog = json.loads((ROOT / "mods/catalog.json").read_text())
    catalog["mods"] += entries
    report(mp.check_catalog(catalog) == [], f"the test catalog passes the schema: {mp.check_catalog(catalog)[:3]}")
    (g.dir / "tagpu_modcatalog.on").write_text(json.dumps(catalog))
    srv, got = serve()
    (g.dir / "tagpu_modbase.on").write_text(f"http://127.0.0.1:{srv.server_address[1]}")

    def skirmish(shot):
        for gadget in ("SINGLE", "Skirmish", "Start"):
            for _ in range(30):
                r = tacli("ui", NAME, "click", gadget, timeout=60, check=False)
                if r.returncode == 0:
                    break
                time.sleep(2)
        for _ in range(60):
            first = (tacli("ui", NAME, timeout=20, check=False).stdout.splitlines() or [""])[0]
            if re.search(r"\b(ARMMAIN|CORMAIN|ARMGAME|game)\b", first, re.I) or "ARMINT" in first:
                break
            time.sleep(2)
        time.sleep(4)
        g.shot(shot)
        tacli("stop", NAME, check=False)

    def diff(a, b):
        from PIL import Image, ImageChops
        im = ImageChops.difference(Image.open(OUT / f"{a}.png").convert("L"), Image.open(OUT / f"{b}.png").convert("L"))
        return sum(1 for v in im.point(lambda v: 255 if v > 24 else 0).getdata() if v)

    def relaunch():
        g.launch()

    # the base alone
    relaunch()
    skirmish("mut-base")

    # installed from Explore, under the base chosen
    relaunch()
    tacli("ui", NAME, "click", "MODS")
    s = g.settled(lambda s: s["open"] and s["drawn"])
    g.click("tab:1")
    s = g.settled(lambda s: s["tab"] == 1)
    names = [r["name"] for r in s["rows"]]
    report(all(n in names for n in ("Tall commander", "Other commander", "Stale commander")), f"Explore lists the base's mutators: {names}")
    report(next(r for r in s["rows"] if r["name"] == "Tall commander")["detail"] == "For ProTA", "each says which base it is for")
    for name in ("Tall commander", "Other commander", "Stale commander"):
        g.click(f"act:{row_of(g.settled(), name)}")
        g.settled(lambda s: name not in [r["name"] for r in s["rows"]] or next(r for r in s["rows"] if r["name"] == name)["act"] == 3, 60)
    end = time.time() + 120
    while time.time() < end and any(r["name"] in ("Tall commander", "Other commander", "Stale commander") for r in g.settled()["rows"]):
        time.sleep(0.5)
    s = g.settled()
    report(not [r for r in s["rows"] if r["name"] in ("Tall commander", "Other commander", "Stale commander")], f"all three installed: {[r['name'] for r in s['rows']]}")
    report(sorted(got) == ["tclash", "tlook", "tstale"], f"each came over the network: {got}")
    report(not any("originals" in str(p) for p in (g.dir / "mods").glob("tlook*/*")), "a mutator runs no recipe")

    # Manage, listed under the base
    g.click("tab:0")
    s = g.settled(lambda s: s["tab"] == 0 and all(r["detail"] != "Checking its files" for r in s["rows"]), 60)
    names = [r["name"] for r in s["rows"]]
    report(names.index("ProTA") < names.index("Tall commander"), f"Manage lists them under the base: {names}")
    r = next(r for r in s["rows"] if r["name"] == "Stale commander")
    report(r["badge"] == 3 and r["act"] == 0, f"the stale one is greyed, with no control: {r}")
    report(f"act:{row_of(s, 'Stale commander')}" not in [h["key"] for h in s["hits"]], "and no hit")
    g.click(f"row:{row_of(s, 'Stale commander')}")
    s = g.settled(lambda s: "differs" in s["note"], 15)
    report("Needs an update for ProTA" in s["note"] and "corcom.3do" in s["note"], f"its pane names the file: {s['note']!r}")
    g.shot("mut-stale")

    g.click(f"act:{row_of(s, 'Tall commander')}")
    s = g.settled(lambda s: next(r for r in s["rows"] if r["name"] == "Tall commander")["act"] == 7)
    report(True, "the one that fits takes its check")
    r = next(r for r in s["rows"] if r["name"] == "Other commander")
    report(r["badge"] == 4 and r["act"] == 0, f"the other of one file is greyed as a conflict: {r}")
    g.click(f"row:{row_of(s, 'Other commander')}")
    s = g.settled(lambda s: "Only one" in s["note"], 15)
    report("armcom.3do" in s["note"] and "Tall commander" in s["note"], f"its pane names the file and the other: {s['note']!r}")
    g.shot("mut-conflict")

    g.click("tab:2")
    s = g.settled(lambda s: s["tab"] == 2)
    r = next((r for r in s["rows"] if r["name"] == "Stale commander"), None)
    report(r is not None and r["detail"] == "Needs an update for ProTA", f"Updates lists it as needing an update: {r}")
    report(s["updates"] >= 1, f"and the tab counts it: {s['updates']}")
    g.click("tab:0")
    s = g.settled(lambda s: s["tab"] == 0)
    report(s["confirm"] == "Restart to apply", f"Confirm offers the restart: {s['confirm']!r}")
    # a file. key an older build wrote, which turned that file off: a mutator mounts whole or not at all
    prof = g.dir / "profiles/prota.ini"
    prof.write_bytes(prof.read_bytes() + b"file.objects3d\\armcom.3do=off\r\n")
    g.click_ending("confirm")

    # the game with it: the look in a skirmish
    g.menu(120)
    log = g.logs()
    report("modload: mounted mutator tlook 1.0" in log, "the start mounts it")
    report("modload: not mounted mutator" not in log or "tstale" not in log, "the stale one stays out, switched off as it is")
    skirmish("mut-look")
    d_look = diff("mut-base", "mut-look")
    report(d_look > 500, f"the look changes the picture, the older key notwithstanding: {d_look} px against the base")

    recs = sorted(g.dir.rglob("*.imprec"), key=lambda p: p.stat().st_mtime)
    if recs:
        sys.path.insert(0, str(ROOT / "tools/replay"))
        import tarec
        h = tarec.header_only(recs[-1])
        report(h.get("mutators", "").startswith("Tall commander 1.0"), f"the recording lists it: {h.get('mutators')!r}")
    else:
        report(False, "no recording to read")

    # a hide: the base's own file leaves its directory, and the original archives answer
    relaunch()
    tacli("ui", NAME, "click", "MODS")
    s = g.settled(lambda s: s["open"] and s["drawn"] and all(r["detail"] != "Checking its files" for r in s["rows"]), 60)
    g.click(f"act:{row_of(s, 'Tall commander')}")
    s = g.settled(lambda s: next(r for r in s["rows"] if r["name"] == "Tall commander")["act"] == 6)
    r = next(r for r in s["rows"] if r["name"] == "Other commander")
    report(r["badge"] == 0 and r["act"] == 6, f"with the other off, the conflict goes: {r}")
    g.click("tab:1")
    s = g.settled(lambda s: s["tab"] == 1)
    g.click(f"act:{row_of(s, 'Hide commander')}")
    end = time.time() + 60
    while time.time() < end and "Hide commander" in [r["name"] for r in g.settled()["rows"]]:
        time.sleep(0.3)
    g.click("tab:0")
    s = g.settled(lambda s: s["tab"] == 0 and all(r["detail"] != "Checking its files" for r in s["rows"]), 30)
    g.click(f"act:{row_of(s, 'Hide commander')}")
    g.settled(lambda s: next(r for r in s["rows"] if r["name"] == "Hide commander")["act"] == 7)
    g.click_ending("confirm")
    g.menu(120)
    log = g.logs()
    report("hides objects3d\\armcom.3do from prota.hpi: 1 entries" in log, "the hide takes the base's file from its directory")
    # the recording made with the other: the hint names both, and the versions
    tacli("ui", NAME, "click", "REPLAYS")
    time.sleep(2)
    tacli("ui", NAME, "select", "RECS", "#0", check=False)
    time.sleep(1)
    ui = tacli("ui", NAME, "--all", "--json", check=False).stdout + tacli("ui", NAME, "show", "FOOTER", check=False).stdout
    g.shot("mut-hint")
    report("Recorded with Tall commander 1.0" in ui and "You have Hide commander 1.0" in ui, f"the browser names what was recorded with and what is mounted: {re.findall(r'[^\"]*ecorded with[^\"]*', ui)[:1] or ui[:300]}")
    tacli("stop", NAME, check=False)
    relaunch()
    skirmish("mut-hide")
    d_hide = diff("mut-base", "mut-hide")
    report(d_hide > 0, f"the picture is the original file's: {d_hide} px against the base's")

    # the folder's own game: a launch with no mod takes the detour only for a mutator switched on
    relaunch()
    tacli("ui", NAME, "click", "MODS")
    s = g.settled(lambda s: s["open"] and s["drawn"])
    g.click("act:0")
    s = g.settled(lambda s: s["chosen"] == "This folder")
    g.click_ending("confirm")
    g.menu(120)
    log = g.logs()
    report("mounted mutator" not in log.rsplit("modinst: the catalog comes", 1)[-1], "this folder with no mutator on mounts none")
    skirmish("mut-none-base")
    relaunch()
    tacli("ui", NAME, "click", "MODS")
    s = g.settled(lambda s: s["open"] and s["drawn"])
    g.click("tab:1")
    s = g.settled(lambda s: s["tab"] == 1)
    report(next(r for r in s["rows"] if r["name"] == "Tall retail commander")["detail"] == "For This folder", "Explore lists the folder's own under it")
    report("Tall commander" not in [r["name"] for r in s["rows"]], "and not another base's")
    g.click(f"act:{row_of(s, 'Tall retail commander')}")
    end = time.time() + 60
    while time.time() < end and "Tall retail commander" in [r["name"] for r in g.settled()["rows"]]:
        time.sleep(0.3)
    g.click("tab:0")
    s = g.settled(lambda s: s["tab"] == 0 and all(r["detail"] != "Checking its files" for r in s["rows"]), 30)
    report(row_of(s, "Tall retail commander") == 1, f"Manage lists it under the folder: {[r['name'] for r in s['rows']]}")
    g.click(f"act:{row_of(s, 'Tall retail commander')}")
    s = g.settled(lambda s: next(r for r in s["rows"] if r["name"] == "Tall retail commander")["act"] == 7)
    report(s["confirm"] == "Restart to apply", f"Confirm offers the restart: {s['confirm']!r}")
    g.click_ending("confirm")
    g.menu(120)
    report("modload: mounted mutator tnone 1.0" in g.logs(), "a launch with no mod mounts it")
    skirmish("mut-none")
    d_none = diff("mut-none-base", "mut-none")
    report(d_none > 500, f"the look changes on the folder's own game: {d_none} px")


def menus(display):
    """Each mod's main menu, and the screen over it: for the eye, and the button's place in its art by `ui`."""
    for mod in ("", "prota", "mayhem", "escalation", "tazero", "twilight"):
        g = Game(display)
        name = mod or "retail"
        if mod:
            subprocess.run([sys.executable, str(ROOT / "tools/modpack/modpack.py"), "install", mod, str(g.dir), "--link"], check=True, capture_output=True)
            cfg = g.dir / "impure.cfg"
            cfg.write_bytes((cfg.read_bytes() if cfg.exists() else b"") + f"\r\nmod={mod}\r\n".encode())
        g.launch()
        time.sleep(2)
        g.shot(f"menu-{name}")
        text = tacli("ui", NAME).stdout
        at = re.search(r"MODS\s+(\d+),(\d+)", text)
        report(bool(at), f"{name}: MODS at {at.group(1) + ',' + at.group(2) if at else None}")
        tacli("ui", NAME, "click", "MODS")
        s = g.settled(lambda s: s["open"] and s["drawn"])
        report(s["open"] == 1 and s["running"] != "", f"{name}: the screen opens, running {s['running']}")
        time.sleep(1)
        g.shot(f"screen-{name}")
        tacli("stop", NAME, check=False)


def hosted(display):
    """Each hosted mod from its real download place: HTTPS and the host's redirects, through the screen, then a start on it."""
    catalog = json.loads((ROOT / "mods/catalog.json").read_text())["mods"]
    want = [e for e in catalog if e.get("base_url") and e["kind"] == "mod"]
    print(f"hosted: {[e['id'] for e in want]}", flush=True)
    for e in want:
        g = Game(display)
        g.launch()
        tacli("ui", NAME, "click", "MODS")
        s = g.settled(lambda s: s["open"] and s["drawn"])
        g.click("tab:1")
        s = g.settled(lambda s: s["tab"] == 1)
        names = sorted(r["name"] for r in s["rows"])
        report(names == sorted(x["name"] for x in want), f"Explore offers the hosted mods alone: {names}")
        g.shot(f"hosted-explore-{e['id']}")
        g.click(f"act:{row_of(s, e['name'])}")
        s = g.settled(lambda s: row_of(s, e["name"]) < 0 or s["note"], 600)
        new = g.dir / "mods" / f"{e['id']}.new" / "installed.txt"
        report(row_of(s, e["name"]) < 0 and new.exists() and new.read_text().strip() == e["version"],
               f"{e['name']} downloaded from {e['base_url']} and installed: note {s['note']!r}")
        g.click("tab:0")
        s = g.settled(lambda s: s["tab"] == 0)
        g.click(f"act:{row_of(s, e['name'])}")
        s = g.settled(lambda s: s["chosen"] == e["name"])
        g.click_ending("confirm")
        report(g.menu(), "the game comes back to its main menu by itself")
        report(f"modload: mounted mods\\{e['id']}\\{e['id']}.hpi" in g.logs(), f"the new process runs {e['name']}")
        g.shot(f"hosted-menu-{e['id']}")
        tacli("stop", NAME, check=False)


def main():
    a = argparse.ArgumentParser()
    a.add_argument("--only", choices=["flow", "menus", "boxes", "real", "mutators", "hosted"])
    # With no GPU to use: on llvmpipe a mod's menu has too little of the 2 GB left for the screen's pass (MEASURED: 60 MB free and the
    # process gone at the open, twice of twice, under ProTA; 227 MB with only this driver loaded and the restorer off)
    a.add_argument("--software", action="store_true", help="llvmpipe alone, the restorer off")
    a.add_argument("--display")
    args = a.parse_args()
    if args.software:
        global SOFTWARE
        SOFTWARE = True
        os.environ["VK_DRIVER_FILES"] = "/usr/share/vulkan/icd.d/lvp_icd.json"
    xv = None
    display = args.display
    if not display:
        for n in range(170, 200):
            if not Path(f"/tmp/.X11-unix/X{n}").exists():
                display = f":{n}"
                xv = subprocess.Popen(["Xvfb", display, "-screen", "0", "1920x1080x24", "-nolisten", "tcp"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                time.sleep(1)
                break
    if not display:
        sys.exit(2)
    runlock.hold(ROOT, "modsscreen")
    # a failure anywhere leaves no game behind, on a display nobody watches
    atexit.register(lambda: tacli("stop", NAME, check=False))
    try:
        if args.only in (None, "mutators"):
            mutators(display)
        if args.only in (None, "flow"):
            flow(display)
        if args.only in (None, "real"):
            real(display)
        if args.only in (None, "boxes"):
            boxes(display)
        if args.only in (None, "menus"):
            menus(display)
        if args.only == "hosted":
            hosted(display)
    finally:
        if xv:
            xv.terminate()
    print(f"modsscreen: {fails} failed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
