#!/usr/bin/env python3
"""modsscreen.py -- the Mods screen played in the game, on Wine: the real DLL, the real button, device clicks.

    python3 tools/mods/modsscreen.py [--only flow|real|boxes|menus] [--display :N] [--software]

`flow` opens the screen from the main menu button and clicks through it: the tabs, a stepper, a switch, the reset, a locked
option, a downloaded Install and Update against a local server, the radio and Confirm, then sees the game restart on the chosen
mod, with the switch in force at its mount, and back again. `real` does a little of that by hardware events on the private display,
in a window larger than the shell, which is the path a player's mouse takes. `boxes` is the screen refused: GDI and an unknown exe. `menus` starts each v1 mod and photographs its main menu and the
screen over it, for the eye: tmp/modsscreen/.

Each control is found in the screen's own state dump (tagpu_modsui.dump), the hit map of the frame last drawn, so a click lands
where the player would aim. It needs the packages tools/modpack/modpack.py packages builds, and starts its own Xvfb when no display is given.
Exit 1 on any failed claim, 2 when it could not run. One game at a time: it holds the machine's run lock.
"""
import argparse
import http.server
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
    report(s["tab"] == 1 and "ProTA" not in names and "Escalation" in names and "Total Mayhem" in names, f"Explore lists what is not installed: {names}")
    g.click(f"row:{row_of(s, 'Escalation')}")
    s = g.settled(lambda s: s["opts"])
    report(all(o["locked"] and o["reason"] for o in s["opts"]), f"Escalation's options are locked with its reason: {s['opts'][0]['reason']}")
    k = [h["key"] for h in s["hits"]]
    report(not any(x.startswith("opt:") for x in k), "a locked option takes no click")
    g.shot("explore-locked")

    g.click(f"act:{row_of(s, 'Total Mayhem')}")
    s = g.settled(lambda s: s["note"], 15)
    report("where to download" in s["note"], f"a catalog that names no place says so: {s['note']!r}")
    (g.dir / "tagpu_modbase.on").write_text(f"http://127.0.0.1:{srv.server_address[1]}")

    g.click("tab:2")
    s = g.settled(lambda s: s["tab"] == 2)
    report(s["updates"] == 1 and row_of(s, "ProTA") == 0 and s["update_all"] == 1, f"Updates finds the old version: {s['rows']}")
    g.shot("updates")

    g.click("tab:0")
    s = g.settled(lambda s: s["tab"] == 0)
    g.click(f"row:{row_of(s, 'ProTA')}")
    s = g.settled(lambda s: s["scan_done"] and s["sw"])
    report(len(s["opts"]) == 2 and not s["opts"][0]["locked"], "ProTA's options show")
    report(any(w["locked"] and w["name"].lower() == "totala1.hpi" for w in s["sw"]), f"the archive every mod needs is locked: {[w['name'] for w in s['sw']]}")
    profile = g.dir / "profiles/prota.ini"
    before = s["opts"][0]["value"]
    g.click("opt:0:-")
    s = g.settled(lambda s: s["opts"][0]["changed"])
    report(s["opts"][0]["changed"] == 1 and s["opts"][0]["value"] != before, f"the stepper moves the radius {before} -> {s['opts'][0]['value']}, and the marker shows")
    report("resource_radius=" in profile.read_text(), "the choice is in profiles\\prota.ini")
    g.click("reset:0")
    s = g.settled(lambda s: not s["opts"][0]["changed"])
    report(s["opts"][0]["value"] == before and "resource_radius" not in profile.read_text(), "the reset takes the key out and the mod's default is back")
    want = next((i for i, w in enumerate(s["sw"]) if w["name"].lower() == "ccdata.ccx"), -1)
    if want < 0:
        want = next(i for i, w in enumerate(s["sw"]) if not w["locked"])
    arc = s["sw"][want]["name"]
    was = s["sw"][want]["on"]
    g.click(f"sw:{want}")
    s = g.settled(lambda s: s["sw"][want]["changed"])
    report(s["sw"][want]["on"] != was and f"hide.{arc}=on" in profile.read_text(), f"the switch hides {arc} in the profile")
    g.shot("manage-prota")
    other = next(i for i, w in enumerate(s["sw"]) if not w["locked"] and i != want)
    g.click(f"sw:{other}")
    s = g.settled(lambda s: s["sw"][other]["changed"])
    g.click(f"swreset:{other}")
    s = g.settled(lambda s: not s["sw"][other]["changed"])
    report(f"hide.{s['sw'][other]['name']}" not in profile.read_text(), "a switch's reset takes its key out")

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
    report(f"not mounted {arc}: your profile hides it" in logs, "the switch is in force at the mount")
    report((g.dir / "mods/prota/installed.txt").read_text().strip() != "0.9", "the update went in at the start")
    tacli("ui", NAME, "click", "MODS")
    s = g.settled(lambda s: s["open"] and s["drawn"])
    report(s["running"] == "ProTA" and s["chosen"] == "ProTA", "the screen says so")
    # the mod running now: its choice takes effect at once, through the profile module's own store
    g.click(f"row:{row_of(s, 'ProTA')}")
    # the scan's end changes the page's shape, and a click across a change of shape is dropped by design
    s = g.settled(lambda s: s["opts"] and s["scan_done"] and s["sw"])
    was = s["opts"][0]["value"]
    g.click("opt:0:+" if was == "0" else "opt:0:-")
    s = g.settled(lambda s: s["opts"][0]["value"] != was, 5)
    report(s["opts"][0]["value"] != was, f"the running mod's option changes at once: {was} to {s['opts'][0]['value']}")
    g.click("reset:0")
    s = g.settled(lambda s: not s["opts"][0]["changed"], 5)
    report(not s["opts"][0]["changed"], "and its reset takes it back")
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
    g.click(f"row:{row_of(s, 'ProTA')}")
    s = g.settled(lambda s: s["scan_done"] and s["sw"] and s["scroll_max"][1] > 0)
    tacli("wheel", NAME, "-3", "--at", "320", "240", check=False)
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
    report((g.dir / "profiles/prota.ini").read_text().count("hide.") == 1, "ProTA's profile survives it")
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


def main():
    a = argparse.ArgumentParser()
    a.add_argument("--only", choices=["flow", "menus", "boxes", "real"])
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
    try:
        if args.only in (None, "flow"):
            flow(display)
        if args.only in (None, "real"):
            real(display)
        if args.only in (None, "boxes"):
            boxes(display)
        if args.only in (None, "menus"):
            menus(display)
    finally:
        if xv:
            xv.terminate()
    print(f"modsscreen: {fails} failed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
