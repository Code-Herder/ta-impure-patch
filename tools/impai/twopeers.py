"""twopeers -- two tacli instances in one network game, for ImpAI's network checks (netai_check.py,
netprobe.py): the run's two instances on a private display and a DirectPlay port leased for the game
(tools/dpport.py), walked into one battle room on Two Continents by tools/mp_lobby.sh, as
tools/replay/recmp_create_begin_test.py's game is. The caller holds the run lock for two game lanes
(tools/runlock.py); the instances carry the run's tag, so one left by a dead run of the tag is replaced.

THE RENDERER. Both peers render with GDI (tareplay.GDI_LEVER in each game folder, which is the
instance's own: launch makes it afresh) and the caller holds no GPU token for them; `gpu` renders
them on the default renderer, for a debugging run, and the caller then holds a token for each.
DIR/renderer.txt names the renderer, and a reading is compared only with readings of its own.

    with TwoPeers(out, dll, port, "na") as tp:
        tp.launch(host_args=[...], join_args=[...], registry={"H": [...], "J": [...]})
        tp.battle_room()
        tp.click(tp.host, "PLAYER2")
        ...
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

TREE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TREE / "tools" / "replay"))
sys.path.insert(0, str(TREE / "tools"))
import dpport  # noqa: E402
import runlock  # noqa: E402
import talog  # noqa: E402
import tareplay  # noqa: E402

MAP = "Two Continents"
STALL_S = 120                           # GameTime standing still this long gives the game up
REC, STRIDE = 0x1B63, 0x14B             # main's player records (tagpu_engine.h)
RECORDS = 10
CouldNotRun = tareplay.CouldNotRun


def renderer(gpu: bool) -> str:
    return "vulkan" if gpu else "gdi"


def field(rec: int, off: int, size: int = 4) -> str:
    """A peek spec of record `rec`'s field at `off`."""
    return f"*0x511DE8+0x{REC + rec * STRIDE + off:X}:{size}"


class TwoPeers:
    """The host (`<token>-<tag>-H`) and the joiner (`...-J`); every tacli call's output in DIR/driver.log;
    each peer stopped, its run log copied to DIR/<instance>.log and the instances removed on the way out.
    `refused` then names each peer whose GPU memory cap refused it an allocation (talog.cap_failure, read
    from its whole log once it has stopped), which fails the check that played them."""

    def __init__(self, out: Path, dll: Path, port, token: str, gpu: bool = False):
        self.out, self.dll, self.want_port, self.token, self.gpu = out, dll, port, token, gpu
        self.root = tareplay.main_checkout()
        self.instances, self.cli = self.root / "tagpu/instances", TREE / "tools/tacli"
        self.owned, self.display, self.lease = [], None, None
        self.refused = []

    def __enter__(self):
        self.lease = dpport.lease(self.want_port)
        self.port = self.lease.port
        self.log = (self.out / "driver.log").open("w")
        print(f"DirectPlay port {self.port}", flush=True)
        try:
            self.display = tareplay.start_xvfb()
            (self.out / "build-sha.txt").write_text(hashlib.sha256(self.dll.read_bytes()).hexdigest() + "\n")
            (self.out / "renderer.txt").write_text(renderer(self.gpu) + "\n")
            tag = f"{self.token}-{runlock.tag()}-"     # an instance of the run's tag is a dead run's
            self.host, self.join = tag + "H", tag + "J"
        except BaseException:
            self.__exit__(*sys.exc_info())
            raise
        return self

    def __exit__(self, *exc):
        for name in self.owned:
            game = self.gamedir(name)
            held = (self.instances / name / "instance.json").exists()
            if held:
                subprocess.run([sys.executable, str(self.cli), "stop", name, "--hard"], stdout=self.log,
                               stderr=subprocess.STDOUT, check=False, timeout=60)
            # read once the peer has stopped, so its last lines are in it
            if (game / "log/tagpu.log").exists():
                text = talog.run_text(game)
                (self.out / f"{name}.log").write_text(text)
                why = talog.cap_failure(text, name)
                if why:
                    self.refused.append(why)
            if held:
                subprocess.run([sys.executable, str(self.cli), "rm", name], stdout=self.log,
                               stderr=subprocess.STDOUT, check=False, timeout=60)
        if self.display:
            tareplay.stop_xvfb(self.display)
        self.lease.close()
        self.log.close()
        return False

    def gamedir(self, name) -> Path:
        return self.instances / name / "gamedir"

    def command(self, *args, timeout=180) -> str:
        r = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=timeout)
        self.log.write(r.stdout + r.stderr)
        self.log.flush()
        if r.returncode:
            raise CouldNotRun(f"command failed ({r.returncode}): {args}; see driver.log")
        return r.stdout

    def tacli(self, *args, timeout=180) -> str:
        return self.command(sys.executable, self.cli, *args, timeout=timeout)

    def peek(self, name, *specs) -> list:
        o = self.tacli("peek", name, *specs, "--json", timeout=60)
        return [int(str(p["value"]).split()[0]) for p in json.loads(o[o.index("{"):])["peeks"]]

    def records(self, name, fields=((0, 4), (4, 4), (0x73, 1), (0x74, 4))) -> dict:
        """{i: (each field's value, in `fields` order)} for the ten records, read in one peek."""
        v = self.peek(name, *(field(i, off, size) for i in range(RECORDS) for off, size in fields))
        k = len(fields)
        return {i: tuple(v[k * i:k * i + k]) for i in range(RECORDS)}

    def launch(self, host_args=(), join_args=(), registry=None):
        """Both instances made fresh and launched for a network game on the leased port, with
        recmp_create_begin_test.py's settings (no commander's death ends the game) and `registry`'s
        values for each (`{"H": ["Name=Value", ...], "J": [...]}`), then at their main menus."""
        registry = registry or {}
        for suffix, name, extra in (("H", self.host, host_args), ("J", self.join, join_args)):
            subprocess.run([sys.executable, str(self.cli), "rm", name, "--force"], stdout=self.log,
                           stderr=subprocess.STDOUT, check=False, timeout=120)
            self.owned.append(name)
            self.tacli("create", name, "--display", self.display.display, "--res", "1024x768")
            shutil.copyfile(self.dll, self.gamedir(name) / "ddraw.dll")
            gdi = self.gamedir(name) / tareplay.GDI_LEVER
            gdi.unlink(missing_ok=True)     # unlinked first: a link is never written through
            if not self.gpu:
                gdi.write_text("")
            self.tacli("registry", name, "MultiCommanderDeath=0", "gamespeed=20", *registry.get(suffix, ()))
            self.tacli("launch", name, "--keep-dll", "--dplay", "--dplay-port", self.port, *extra)
            self.tacli("ui", name, "wait", "--gui", "MAINMENU", "--timeout", "45")

    def battle_room(self):
        """Both peers in one battle room on MAP, nobody ready."""
        subprocess.run(["bash", str(TREE / "tools/mp_lobby.sh"), "--map", MAP, *self.owned], stdout=self.log,
                       stderr=subprocess.STDOUT, check=True, timeout=900, env=dict(os.environ, MP_NO_START="1"))

    def click(self, name, gadget, timeout=None):
        self.tacli("ui", name, "click", gadget, *(("--timeout", str(timeout)) if timeout else ()))

    def add_ais(self):
        """One computer seat a peer: the host's row 2 (Unused, Blocked, AI) and the joiner's row 3."""
        for _ in range(2):
            self.click(self.host, "PLAYER2")
        self.click(self.join, "PLAYER3")

    def start(self):
        """Both ready, the host's Start, both in play."""
        for name in self.owned:
            self.click(name, "READY0")
        self.click(self.host, "START", 30)
        for name in self.owned:
            self.tacli("wait", name, "alive=[1-9]", "--timeout", "300", timeout=360)

    def gametime(self, name) -> int:
        return self.peek(name, tareplay.GAMETIME)[0]

    def play_to(self, until):
        """Until the host's GameTime reaches `until`; STALL_S of none gives the game up."""
        last, moved = -1, time.monotonic()
        while True:
            now = self.gametime(self.host)
            if now >= until:
                return now
            if now != last:
                last, moved = now, time.monotonic()
            elif time.monotonic() - moved > STALL_S:
                raise CouldNotRun(f"the host's GameTime stood at {now} for {STALL_S} s")
            time.sleep(2)

    def leave(self, name):
        """The peer back at its main menu through the game's own menus, so its teardown runs."""
        gui = json.loads(self.tacli("ui", name, "--json"))["gui"].upper()
        if gui == "ENDMSN.GUI":
            self.tacli("ui", name, "click", "MainMenu", "--timeout", "30")
        elif gui != "MAINMENU.GUI":
            self.command("bash", TREE / "tools/mp_leave.sh", name, timeout=300)

    def run_log(self, name) -> str:
        return talog.run_text(self.gamedir(name))
