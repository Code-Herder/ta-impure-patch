#!/usr/bin/env python3
"""Native-input assertions for the resource replay fixtures.

The live lane must produce real build/reclaim/guard orders before tareplay
compares it with playback, restoration and fresh-process seeks. Memory reads
are measurements through tacli's game-thread peek; all actions use its input.

IN LOCKSTEP: a case is handed the world held (tareplay's run step, on a test
launch's own pacing: tagpu_replay.c THE LAUNCH'S HOLD and THE PAUSE AT T),
advances it by exact ticks (`pause at` and `resume`) and hands it back held
where it left it. Every click and key lands on a held world, so the world a
click meets is a function of the ticks the case asked for, never of wall-clock
pacing; an input's direct effect is read once after `tacli keys` returns (it
returns once the engine has consumed every event); simulation progress is read
after each tick of an explicit, bounded advance. A world that is not held is
refused: there is no tick a running world could be stopped at for certain.
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import time
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path

TREE = Path(__file__).resolve().parents[1]
TACLI = TREE / "tools/tacli"
GAMETIME = "*0x511DE8+0x38a47"
PAUSED = "*0x511DE8+0x38a51:2"   # bit 0: the pause the Pause key flips, and THE PAUSE AT T sets
TRIGGER = "tagpu_replay.trigger"
ACK_S = 60                       # a frame's answer: a trigger's line, the camera, a menu (tareplay)
STALL_S = 120                    # a running level's GameTime standing still: a hung game (tareplay)


def tacli_api():
    """tacli as a module, for its instance paths and its log reader (as tareplay loads it)."""
    spec = spec_from_loader("_resource_cli", SourceFileLoader("_resource_cli", str(TACLI)))
    api = module_from_spec(spec)
    spec.loader.exec_module(api)
    return api


FIXTURE_UNITS = {"ARMCK", "ARMACK", "CORCK", "ARMPW", "ARMMEX"}


class Check:
    def __init__(self, name, need=FIXTURE_UNITS):
        """The units the case needs (`need`, the player's types) in the roster. The viewer needs
        its constructor only: it runs at the twin, by which the fixture's own case may have
        upgraded the mex away (resource-refusals reclaims it for a Moho from GameTime 121)."""
        self.name = name
        self.lockstep()
        self.base = self.values("*0x511DE8+0x14357")[0]
        def loaded():
            roster = self.call("roster", "--json")["units"]
            units = {u["type"].upper(): u["engine_index"] for u in roster if u["owner"] == 0}
            return units if set(need) <= units.keys() else None
        self.units = self.until(loaded, "resource fixture units have not reached the roster")

    # -- lockstep: the world held, advanced by exact ticks
    def lockstep(self):
        """The trigger file and a log reader from now on; the world not yet held."""
        api = tacli_api()
        instance = api.Instance(self.name)
        self.gamedir = instance.gamedir
        self.reader = api._LogLines(api._log_cursor(instance))
        self.log = []
        self.held = False

    def _found(self, rx, start):
        """The first log line from index `start` on matching `rx`, None while there is none."""
        self.log += [ln.rstrip() for ln in self.reader()]
        return next((ln for ln in self.log[start:] if re.search(rx, ln)), None)

    def _answer(self, rx, start, seconds=ACK_S):
        """A frame's answer: the first log line from index `start` on matching `rx`."""
        end = time.monotonic() + seconds
        while (got := self._found(rx, start)) is None:
            assert time.monotonic() < end, f"no line matching {rx!r} in {seconds} s"
            time.sleep(0.02)
        return got

    def _write(self, line):
        """One line of the replay trigger, written whole and renamed into place as tareplay writes
        it; the log index its answer is looked for from. Each caller awaits the answer before the
        next line, so no line replaces one the DLL has not read."""
        self.log += [ln.rstrip() for ln in self.reader()]
        tmp = self.gamedir / (TRIGGER + ".tmp")
        tmp.write_text(line + "\n")
        os.replace(tmp, self.gamedir / TRIGGER)
        return len(self.log)

    def _resume(self):
        start = self._write("resume")
        got = self._answer(r"pause: resumed at GameTime \d+|resume: refused: .*", start)
        assert got.startswith("pause: resumed"), f"resume: {got}"

    def _pause_at(self, T):
        """`pause at T` armed, T past the held tick; the index its taking is looked for from."""
        start = self._write(f"pause at {T}")
        got = self._answer(rf"pause: (armed for GameTime {T}$|at {T} refused: .*)", start)
        assert "armed for" in got, f"pause at {T}: {got}"
        return start

    def _taken(self, T, start):
        """The pause at T taken. The level runs until then, as many ticks as the case advances, so
        the wait is bounded by its progress: it gives up only when GameTime stands still for
        STALL_S, a hung game, and a loaded machine makes it longer, never different."""
        rx = rf"pause: (paused at GameTime {T}$|at {T} not taken.*)"
        tick, since = None, time.monotonic()
        while (got := self._found(rx, start)) is None:
            now = self.values(GAMETIME)[0]
            if now != tick:
                tick, since = now, time.monotonic()
            assert time.monotonic() - since < STALL_S, \
                f"pause at {T}: GameTime stood at {tick} for {STALL_S} s"
            time.sleep(0.05)
        assert "paused at" in got, got
        self.held, self.tick = True, T

    def _still_held(self):
        """The hold's pause bit is still set: nothing let the world run under a case that holds it
        (`resume` checks only that the DLL took a pause)."""
        assert self.values(PAUSED)[0] & 1, f"the world held at GameTime {self.tick} ran free"

    def hold(self):
        """The world as it was handed over, held: the case's lockstep starts at its GameTime. The
        pause bit is the hold's (THE LAUNCH'S HOLD or THE PAUSE AT T, which the hold keeper keeps
        set until `resume`), and both values are read in one peek, at one flip."""
        tick, paused = self.values(GAMETIME, PAUSED)
        assert paused & 1, f"the world was handed over running at GameTime {tick}: a case needs it held"
        self.held, self.tick = True, tick

    def advance(self, n=1):
        """Exactly `n` simulation ticks, and held again: `pause at` the held tick plus n, then
        `resume` (which clears only a pause the DLL's pacing took)."""
        assert self.held, "advance needs the world held"
        self._still_held()
        T = self.tick + n
        start = self._pause_at(T)
        self._resume()
        self._taken(T, start)

    def release(self):
        """The world handed back held where the case left it, as tareplay's lane handed it over."""
        if self.held:
            self._still_held()
            self.held = False

    def call(self, command, *args):
        # past tacli's own 90 s bound on an input batch, so tacli abandons a stalled one itself
        r = subprocess.run([sys.executable, str(TACLI), command, self.name, *map(str, args)],
                           capture_output=True, text=True, timeout=150)
        assert r.returncode == 0, r.stdout + r.stderr
        return json.loads(r.stdout) if "--json" in args else r.stdout

    def values(self, *specs):
        return [int(p["value"].split()[0]) for p in self.call("peek", *specs, "--json")["peeks"]]

    def bytes(self, spec):
        s = self.call("peek", spec, "--json")["peeks"][0]["value"]
        return bytes.fromhex(s)

    def snapshot(self, *specs):
        """The specs' values, all read at one instant: tagpu_peek_frame evaluates a trigger's
        every line in one call on the game thread (tagpu_triggers_frame, once per engine flip),
        where no simulation step can run between two of them. A row missing or naming another
        spec means the DLL did not evaluate, whole, the trigger tacli wrote."""
        rows = self.call("peek", *specs, "--json")["peeks"]
        assert [r["spec"] for r in rows] == list(specs), f"peek answered {len(rows)} of {len(specs)} specs"
        return [r["value"] for r in rows]

    def until(self, function, message, seconds=ACK_S, ticks=None, step=1):
        """Until `function` answers. Without `ticks`, polled for at most `seconds` of wall clock:
        the camera and the menus, which no simulation step drives and a frame or a few answer, so
        the bound only catches a game that stopped answering. With `ticks`, simulation progress
        in lockstep: read on the held world, then after each advance of `step` ticks, at most
        `ticks` ticks in all."""
        if ticks is None:
            end = time.monotonic() + seconds
            while True:
                result = function()
                if result:
                    return result
                assert time.monotonic() < end, message
                time.sleep(0.1)
        for done in range(0, ticks + 1, step):
            result = function()
            if result:
                return result
            if done + step <= ticks:
                self.advance(step)
        raise AssertionError(f"{message} in {ticks} ticks")

    @staticmethod
    def once(result, message):
        """An input's direct effect, read once: `tacli keys` returns once the engine has consumed
        every event (tagpu_input.c), and orders, selection, the placement rectangle and the cursor
        mode are written while the event is handled or in the frame drawn before its
        acknowledgement."""
        assert result, message
        return result

    def address(self, kind):
        return self.base + self.units[kind] * 0x118

    def chain(self, kind):
        """The unit's order nodes, 0x50 bytes each, as the engine holds them at one instant,
        paused or running.

        A running game can finish an order and hand its node to another unit between two reads,
        so no node is taken from a read that did not also take the head and every node before it.
        Each round reads the head (unit +0x5C) and every node found so far in one snapshot and
        follows the links that snapshot holds; a link to a node it did not read becomes the next
        round's last read. The chain is the engine's when one snapshot links the head to a null
        +0x4A through nodes it read itself."""
        owner = self.address(kind)
        head = f"0x{owner + 0x5c:x}"
        nodes = []
        for _ in range(200):
            values = self.snapshot(head, *(f"0x{n:x}:x50" for n in nodes))
            # A node an earlier round found may since have been freed with its page; that
            # costs nothing unless this snapshot still links to it.
            read = {n: bytes.fromhex(v) for n, v in zip(nodes, values[1:]) if not v.startswith("<")}
            node, linked = int(values[0].split()[0]), []
            while node in read:
                assert node not in linked and len(linked) < 100, "cyclic/excessive fixture order chain"
                linked.append(node)
                node = struct.unpack_from("<I", read[node], 0x4a)[0]
            assert node not in nodes, f"{kind}'s order chain links unreadable memory"
            if not node:
                break
            nodes = linked + [node]
        else:
            raise AssertionError(f"{kind}'s order chain changed under every read")
        for n in linked:
            assert struct.unpack_from("<I", read[n], 0x0e)[0] == owner, "wrong order owner"
        return [read[n] for n in linked]

    def orders(self, kind):
        return [{"action": b[4], "target": struct.unpack_from("<I", b, 0x16)[0],
                 "pos": struct.unpack_from("<iii", b, 0x22),
                 "type": struct.unpack_from("<H", b, 0x36)[0]} for b in self.chain(kind)]

    def placed(self, orders):
        if not orders:
            return []
        descriptors = self.values("0x512344")[0]
        caps = self.values(*(f"0x{descriptors + o['action'] * 0x19 + 0xc:x}" for o in orders))
        return [o for o, cap in zip(orders, caps) if cap & 1 and o["type"]]

    def stop(self, kind):
        self.call("ui", "click", "ARMSTOP")
        self.until(lambda: self.mode() == 1 and all(o["action"] == 41 for o in self.orders(kind)),
                   "Stop did not restore contextual idle state", ticks=30)

    def keys(self, *tokens):
        self.call("keys", *tokens)

    def eye(self, x, z):
        self.call("eye", x, z)
        self.until(lambda: self.values("*0x511DE8+0x1431f", "*0x511DE8+0x14323") == [x, z],
                   "camera did not reach the fixture eye")

    def screen(self, kind):
        b = self.bytes(f"0x{self.address(kind) + 0x6a:x}:xc")
        x, h, z = (v >> 16 for v in struct.unpack("<iii", b))
        ex, ez = self.values("*0x511DE8+0x1431f", "*0x511DE8+0x14323")
        return x - ex + 128, z - h // 2 - ez + 32

    def select(self, *kinds):
        self.keys("esc")
        eye = self.values("*0x511DE8+0x1431f", "*0x511DE8+0x14323")
        for i, kind in enumerate(kinds):
            x, z = self.screen(kind)
            if not (128 < x < 1024 and 32 < z < 744):
                b = self.bytes(f"0x{self.address(kind) + 0x6a:x}:xc")
                wx, _, wz = struct.unpack("<iii", b)
                self.eye((wx >> 16) - 300, (wz >> 16) - 300)
                x, z = self.screen(kind)
            self.keys(*(["down:shift"] if i else []), f"pclick:{x},{z}",
                      *(["up:shift"] if i else []))
        self.once(all(self.values(f"0x{self.address(k) + 0x110:x}")[0] & 0x10 for k in kinds),
                  f"selection did not reach fixture units {kinds}")
        self.eye(*eye)

    def mode(self):
        return self.values("*0x511DE8+0x2cc3:1")[0]

    def rect(self):
        return struct.unpack("<iiiiii", self.bytes("*0x511DE8+0x2c92:x18"))

    def menu(self, **settings):
        self.call("arm", "menu.open")
        for name, value in settings.items():
            self.call("ui", "set", name, value)
        self.call("arm", "menu.open=off")
        self.until(lambda: self.call("ui", "--json")["gui"] != "RENDER.GUI",
                   "Impure options did not close")

    def build(self, name):
        snap = self.call("ui", "--json")
        for g in snap["gadgets"]:
            if g["name"].endswith("BUILD"):
                self.call("ui", "click", g["name"])
                break
        for _ in range(8):
            gadgets = self.call("ui", "--json")["gadgets"]
            if any(g["name"] == name for g in gadgets):
                self.call("ui", "click", name)
                return
            pager = next(g["name"] for g in gadgets if g["name"].endswith("NEXT"))
            self.call("ui", "click", pager)
        raise AssertionError(f"{name} absent from the fixture builder's menu")

    def resource_click(self, x, z, shift=False):
        right = self.values("*0x511DE8+0x37efa")[0] != 0
        self.keys(*(["down:shift"] if shift else []),
                  f"{'prclick' if right else 'pclick'}:{x},{z}",
                  *(["up:shift"] if shift else []))

    def repeat(self, x, z, shift=False):
        def count():
            return len(self.call("log", "--grep", "resource: already queued", "--tail", 100, "--json")["lines"])
        before = count()
        self.resource_click(x, z, shift=shift)
        self.once(count() > before, "repeat click was not consumed as already queued")

    def unqueue(self, x, z, message="Shift-click did not cancel the queued resource plan"):
        def count():
            return len(self.call("log", "--grep", "resource: unqueued", "--tail", 100, "--json")["lines"])
        # Both queue images must reach recorder capture points: a tick before the click and
        # one after it. A render frame or the click's acknowledgement alone is none.
        self.advance(1)
        before = count()
        self.resource_click(x, z, shift=True)
        self.once(count() > before, message)
        self.advance(1)

    def guard(self, kind, target):
        """Give `kind` the preceding order a Shift-click appends after, a Guard of an owned unit
        that is neither damaged, building nor attacked, and return once the Guard has taken its
        first step.

        A Move completes, on the generated maps (1024 world units across) within the time a case
        runs. Such a Guard does not, and its compared fields are fixed from its first step on
        [DISASSEMBLED, exe-reverse-engineering.md, *The ground Guard's node*]: that step, state 0
        of Follow_Ground 0x406300, writes the position +0x22..+0x2D as a random offset around the
        target and +0x36 as the follow radius, no later step writes them, and the order dispatch
        at 0x43B87C returns a node to state 0 only on a result Follow_Ground never gives. So a
        queue read once the state byte +5 has left 0 compares equal to one read after a
        cancellation, however many ticks pass between them."""
        self.call("order", "guard", "unit", self.units[target], "--unit", self.units[kind],
                  "--expect", kind, "--expect-target", target)
        self.stepped(kind, target, f"{kind}'s Guard of {target} took no first step")

    def occupy(self, kind, order):
        """`kind`, unselected, ordered onto the footprint of the build `order` and advanced until
        the engine's grid holds its index in a cell of that footprint: the ground the site test
        0x47D2E0 refuses (a cell's unit word +0x00, the 13-byte cells at main+0x14287, row width
        main+0x14233)."""
        defs = self.values("*0x511DE8+0x1439b")[0]
        fx, fz = struct.unpack("<hh", self.bytes(f"0x{defs + order['type'] * 0x249 + 0x14a:x}:x4"))
        px, pz = order["pos"][0] >> 16, order["pos"][2] >> 16
        x0, z0 = (px - fx * 8) // 16, (pz - fz * 8) // 16
        grid, width = self.values("*0x511DE8+0x14287", "*0x511DE8+0x14233")
        rows = [f"0x{grid + ((z0 + dz) * width + x0) * 13:x}:x{13 * fx}" for dz in range(fz)]
        index = self.units[kind]
        def stamped():
            return any(struct.unpack_from("<H", bytes.fromhex(v), 13 * dx)[0] == index
                       for v in self.snapshot(*rows) for dx in range(fx))
        self.call("order", "move", "pos", px - 8 * fx, pz - 8 * fz, "--unit", index,
                  "--expect", kind)
        self.until(stamped, f"{kind} never stood on the queued site", ticks=900, step=15)

    def stepped(self, kind, target, message):
        """Return once `kind` holds a Guard of `target` past its first step (state +5 not 0): the
        point from which its compared fields are fixed (guard)."""
        guarded = self.address(target)
        self.until(lambda: any(struct.unpack_from("<I", b, 0x16)[0] == guarded and b[5]
                               for b in self.chain(kind)), message, ticks=90)


def mex(c, classic=False):
    if classic:
        # Launch forces right-button orders. Select the native interface
        # after launch so this fixture actually reaches the left-click gate.
        c.keys("tab")
        c.call("ui", "click", "PREFS")
        c.call("ui", "click", "SPEEDS")
        c.call("ui", "set", "LEFTCLICK", 0)
        c.call("ui", "click", "PREV")
        c.call("ui", "click", "OK")
    c.until(lambda: c.values("*0x511DE8+0x37efa")[0] == int(not classic),
            "mex regression did not reach its required mouse interface")
    c.hold()
    c.eye(1000, 950)
    c.menu(RESOURCE=3, QUICKBUILD=0)
    c.select("ARMCK")
    c.call("ui", "click", "ARMMEX")
    c.keys("pmove:588,377")
    snapped = c.once(c.rect() if c.mode() == 14 and c.rect()[0] == 1408 else None,
                     "manual mex did not snap to the real deposit")
    c.keys("down:alt")
    c.once(c.rect()[0] == 1440, "stationary Alt did not bypass manual snapping")
    c.keys("up:alt")
    c.once(c.rect() == snapped, "manual snapping did not resume after Alt")
    c.keys("pmove:620,375")
    c.once(c.rect()[0] == 1472, "manual snapping accepted a partial deposit at the range boundary")
    c.keys("pmove:588,377")
    c.once(c.rect() == snapped, "manual snapping did not return to the deposit center")
    c.keys("down:shift", "pclick:588,377", "up:shift")
    def built():
        orders = c.orders("ARMCK")
        return orders if any(o["type"] == 78 and o["pos"][0] >> 16 == 1432 for o in orders) else None
    orders = c.once(built(), "manual snapped build produced no order")
    assert any(o["pos"][0] >> 16 == 1432 for o in orders), "manual order lost its snapped position"
    # Stop leaves manual placement before the independent Quick Build click.
    c.menu(RESOURCE=0, QUICKBUILD=1)
    c.stop("ARMCK")
    c.resource_click(620, 375)
    partial = c.once(c.orders("ARMCK") if any(o["action"] != 41 for o in c.orders("ARMCK")) else None,
                     "snap-range boundary click produced no contextual order")
    assert not c.placed(partial), "Quick Build accepted a partial deposit at the snap-range boundary"
    c.stop("ARMCK")
    c.resource_click(588, 377)
    orders = c.once(built(), "Quick Build did not match the manual snapped placement")
    centers = {(o["pos"][0] >> 16, o["pos"][2] >> 16) for o in c.placed(orders)}
    assert centers == {((snapped[0] + snapped[3]) // 2, (snapped[2] + snapped[5]) // 2)}, \
        "Quick Build and the manual ghost selected different deposit centers"
    c.stop("ARMCK")
    # A preceding Guard keeps the plan unstarted by queue ordering; the walk to the storage
    # stays 250 world units south of the deposits clicked. Its first step is advanced to;
    # the world stays held at it for every click that follows.
    c.guard("ARMCK", "ARMMSTOR")
    preceding = c.orders("ARMCK")
    c.resource_click(560, 375, shift=True)
    orders = c.once(built(), "Quick Build failed with manual radius off")
    c.repeat(560, 375)
    assert c.orders("ARMCK") == orders, "repeat click changed the queued mex orders"
    # Another selected constructor must not cancel an unselected owner's plan.
    c.select("CORCK")
    other = c.orders("CORCK")
    c.repeat(560, 375, shift=True)
    assert c.orders("ARMCK") == orders and c.orders("CORCK") == other, "unqueue changed an unselected constructor or added an order"
    # The strongest available variant is now Moho, but the queued node is MEX.
    c.select("ARMCK", "ARMACK")
    advanced = c.orders("ARMACK")
    c.unqueue(560, 375)
    assert c.orders("ARMCK") == preceding, "unqueue removed the preceding Guard or kept the mex"
    assert c.orders("ARMACK") == advanced, "unqueue added an order to a constructor without the plan"
    c.select("ARMCK")
    c.stop("ARMCK")
    c.resource_click(560, 375)
    c.once(built(), "cancelled mex could not be queued again")
    # A queued plan is the plan's whatever stands on its site (tagpu_resource.c site): with a
    # unit stopped on its footprint, the Shift-click still cancels it rather than leaving the
    # engine to append an order in its place.
    c.stop("ARMCK")
    c.guard("ARMCK", "ARMMSTOR")
    preceding = c.orders("ARMCK")
    c.resource_click(560, 375, shift=True)
    plan = c.once([o for o in c.placed(c.orders("ARMCK")) if o["type"] == 78],
                  "the mex to stand on was not queued")
    c.occupy("ARMPW", plan[0])
    c.select("ARMCK", "ARMACK")
    c.unqueue(560, 375, "Shift-click did not cancel a queued plan with a unit on its site")
    assert c.orders("ARMCK") == preceding, "Shift-click on an occupied queued site did not cancel only its plan"
    assert c.mode() == 1, "Quick Build left contextual mode"
    print(f"resource-mex: {'left' if classic else 'right'} interface, center parity, snap boundary, Alt, independent toggle, plain no-op, selected-only variant-safe Shift cancellation and requeue, cancellation on an occupied site passed")


def upgrade(c, shift=False):
    c.hold()
    c.eye(1250, 1200)
    c.menu(RESOURCE=3, QUICKBUILD=1)
    c.select("ARMACK", "ARMCK", "CORCK", "ARMPW")
    if shift:
        # ARMACK and ARMCK are the owned units east of the clicked mex, and ARMCK, a helper,
        # guards ARMACK back: the two close on each other and neither walk crosses the mex.
        c.guard("ARMACK", "ARMCK")
        preceding = c.orders("ARMACK")
    combat = c.orders("ARMPW")
    x, z = c.screen("ARMMEX")
    c.resource_click(x, z, shift=shift)
    def upgraded():
        orders = c.orders("ARMACK")
        return orders if any(o["type"] == 88 for o in orders) else None
    orders = c.once(upgraded(), "upgrade produced no orders")
    offset = int(shift)
    assert len(orders) >= offset + 2 and orders[offset]["target"] == c.address("ARMMEX"), "missing reclaim before upgrade"
    assert orders[offset + 1]["type"] != 0 and orders[offset + 1]["target"] == 0, "missing upgrade build after reclaim"
    for helper in ("ARMCK", "CORCK"):
        def guarded():
            h = c.orders(helper)
            return h if any(o["target"] == c.address("ARMACK") for o in h) else None
        c.once(guarded(), f"{helper} received no assist order")
        # a helper's Guard is compared later too: read it once its first step has fixed it
        c.stepped(helper, "ARMACK", f"{helper}'s assist took no first step")
        h = c.orders(helper)
        assert sum(o["target"] == c.address("ARMACK") for o in h) == 1, f"{helper} did not guard the primary"
    assert c.orders("ARMPW") == combat, "Quick Build changed a selected combat unit's orders"
    # read on the tick the repeat lands on: waiting for the helpers' first steps advanced the
    # world, and ARMACK's new reclaim/build pair steps with it
    queued = c.orders("ARMACK")
    c.repeat(x, z)
    assert c.orders("ARMACK") == queued, "repeat upgrade toggled or duplicated its queue"
    for helper in ("ARMCK", "CORCK"):
        assert sum(o["target"] == c.address("ARMACK") for o in c.orders(helper)) == 1, "repeat upgrade duplicated a helper guard"
    if shift:
        helpers = {k: c.orders(k) for k in ("ARMCK", "CORCK")}
        c.unqueue(x, z)
        assert c.orders("ARMACK") == preceding, "unqueue did not remove only the reclaim/build pair"
        assert all(c.orders(k) == orders for k, orders in helpers.items()), "unqueue changed persistent helpers"
        assert c.orders("ARMPW") == combat, "unqueue changed selected combat orders"
        c.resource_click(x, z, shift=True)
        c.once(upgraded(), "cancelled upgrade could not be queued again")
    assert c.mode() == 1, "upgrade left contextual mode"
    print("resource-upgrade: reclaim/build sequence, mixed helpers, combat priority and repeat no-op passed")


def terrain(c, mode):
    c.hold()
    c.eye(100, 150)
    c.menu(RESOURCE=3, QUICKBUILD=1)
    c.select("ARMCK")
    # The generated map is flat height 32. These clicks target cell (23,23),
    # which has a discrete deposit only in the mixed map.
    x, z = 23 * 16 + 8 - 100 + 128, 23 * 16 + 8 - 16 - 150 + 32
    c.resource_click(x, z)
    def ordered():
        orders = c.orders("ARMCK")
        return orders if orders and orders[0]["action"] != 41 else None
    orders = c.once(ordered(), "metal-map contextual click did nothing")
    if mode == "mixed":
        assert any(o["type"] == 78 for o in orders), "rich deposit on metal ground was not built"
    else:
        assert not any(o["type"] == 78 for o in orders), "uniform metal ground became a Quick Build target"
    # Geo remains eligible on every metal-map kind and manual geo snapping
    # feeds the same ordinary build-order recorder root.
    c.select("CORCK")
    c.build("CORGEO")
    c.keys("pmove:708,258")
    def vent():
        r = c.rect()
        green = c.values("*0x511DE8+0x2cc6:1")[0] & 0x40
        return r if c.mode() == 14 and green else None
    snapped = c.once(vent(), "manual geothermal snap did not find the generated vent")
    c.keys("down:alt")
    c.once(c.rect() != snapped, "Alt did not bypass geothermal snapping")
    c.keys("up:alt")
    c.once(c.rect() == snapped, "geothermal snap did not resume")
    c.keys("esc")
    c.guard("CORCK", "ARMMSTOR")
    preceding = c.orders("CORCK")
    c.resource_click(676, 258, shift=True)
    c.once(any(o["type"] == 193 for o in c.orders("CORCK")), "Quick geo produced no geothermal build order")
    c.unqueue(676, 258)
    assert c.orders("CORCK") == preceding, "geo unqueue did not preserve the preceding Guard"
    c.resource_click(676, 258, shift=True)
    c.once(any(o["type"] == 193 for o in c.orders("CORCK")), "cancelled geo could not be queued again")
    print(f"resource-{mode}: metal-ground classification, manual geo/Alt, Quick geo cancellation and requeue passed")


def policy(c, mode, gamedir):
    c.hold()
    c.eye(1000, 950)
    c.call("arm", "menu.open")
    def opened():
        snap = c.call("ui", "--json")
        controls = {g["name"]: g for g in snap["gadgets"]}
        return controls if snap["gui"] == "RENDER.GUI" and {"RESOURCE", "QUICKBUILD"} <= controls.keys() else None
    controls = c.until(opened, "resource policy options did not open")
    assert controls["QUICKBUILD"]["grayed"] == 1, "mod profile did not lock Quick Build off"
    assert controls["QUICKBUILD"]["stage"] == 0, "locked Quick Build stayed on"
    if mode == "locked":
        assert controls["RESOURCE"]["grayed"] == 1 and controls["RESOURCE"]["stage"] == 0, "mod profile did not lock manual snapping off"
    else:
        assert controls["RESOURCE"]["grayed"] == 0 and controls["RESOURCE"]["stage"] <= 1, "version override did not inherit and clamp manual snapping"
    # Neither editing general rules nor removing an override may change the
    # policy once this match has started, including while its options are open.
    (gamedir / "profiles/retail.ini").write_text("manual_snapping=1\nquick_build=1\n"
                                              "default_radius=3\nmax_radius=3\nquick_build_default=1\n")
    (gamedir / "profiles/retail@76332e31.ini").unlink(missing_ok=True)
    c.values("*0x511DE8+0x38a47")
    controls = {g["name"]: g for g in c.call("ui", "--json")["gadgets"]}
    assert controls["QUICKBUILD"]["grayed"] == 1, "profile edit changed an in-progress match"
    c.call("arm", "menu.open=off")
    c.until(lambda: c.call("ui", "--json")["gui"] != "RENDER.GUI", "options did not close")
    c.select("ARMCK")
    c.resource_click(560, 375)
    c.once(any(o["action"] != 41 for o in c.orders("ARMCK")), "locked contextual click did nothing")
    assert not any(o["type"] == 78 for o in c.orders("ARMCK")), "locked Quick Build issued a mex order"
    print(f"resource-policy-{mode}: locks, inheritance/clamping and match freeze passed")


def geo_upgrade(c):
    c.hold()
    c.eye(100, 150)
    c.menu(RESOURCE=3, QUICKBUILD=1)
    c.select("CORCK", "ARMCK")
    types = c.call("units", "--json")["units"]
    stronger = next(t["id"] for t in types if t["name"].upper() == "RSUPGEO")
    c.guard("CORCK", "ARMMSTOR")
    preceding = c.orders("CORCK")
    x, z = c.screen("CORGEO")
    c.resource_click(x, z, shift=True)
    def upgraded():
        orders = c.orders("CORCK")
        return orders if any(o["type"] == stronger for o in orders) else None
    orders = c.once(upgraded(), "geothermal upgrade did not append a stronger plant")
    reclaim = next(i for i, o in enumerate(orders) if o["target"] == c.address("CORGEO"))
    assert orders[reclaim + 1]["type"] == stronger, "geo upgrade lost reclaim/build ordering"
    c.repeat(x, z)
    assert c.orders("CORCK") == orders, "repeat geo upgrade altered its queue"
    c.stepped("ARMCK", "CORCK", "geo helper received no persistent guard past its first step")
    helper = c.orders("ARMCK")
    c.unqueue(x, z)
    assert c.orders("CORCK") == preceding, "geo unqueue did not remove only the reclaim/build pair"
    assert c.orders("ARMCK") == helper, "geo unqueue changed the persistent helper"
    c.resource_click(x, z, shift=True)
    c.once(upgraded(), "cancelled geo upgrade could not be queued again")
    print("resource-geo-upgrade: stronger variant, Shift reclaim/build and persistent helper passed")


def nearby(c):
    c.hold()
    c.eye(100, 150)
    c.menu(RESOURCE=3, QUICKBUILD=1)
    c.select("CORCK")
    types = c.call("units", "--json")["units"]
    geo = next(t["id"] for t in types if t["name"].upper() == "RSGEO")
    c.guard("CORCK", "ARMMSTOR")
    def sites():
        return {(o["pos"][0] >> 16, o["pos"][2] >> 16)
                for o in c.placed(c.orders("CORCK")) if o["type"] == geo}
    c.resource_click(676, 258, shift=True)
    c.once(sites() == {(648, 392)}, "first one-cell vent was not queued")
    c.advance(1)
    c.resource_click(692, 258, shift=True)
    c.once(sites() == {(648, 392), (664, 392)},
           "queueing the neighboring vent toggled or replaced the first plan")
    c.unqueue(692, 258)
    assert sites() == {(648, 392)}, "unqueue cancelled the neighboring vent instead of its own plan"
    assert any(o["target"] == c.address("ARMMSTOR") for o in c.orders("CORCK")), "unqueue removed the preceding Guard"
    c.resource_click(692, 258, shift=True)
    c.once(sites() == {(648, 392), (664, 392)}, "neighboring vent could not be requeued")
    c.advance(1)
    print("resource-nearby: distinct adjacent vents, Shift append/cancel/requeue, preceding Guard and recorded cancellation passed")


def priority(c):
    c.hold()
    c.eye(1000, 950)
    c.menu(RESOURCE=3, QUICKBUILD=1)
    c.select("ARMCK")
    for case in ("move", "alt", "equal", "off"):
        c.stop("ARMCK")
        if case == "move":
            c.call("ui", "click", "ARMMOVE")
            c.once(c.mode() != 1, "explicit Move mode did not arm")
        if case == "alt":
            c.keys("down:alt")
        if case == "off":
            c.menu(QUICKBUILD=0)
        if case == "equal":
            c.eye(1250, 1200)
            x, z = c.screen("ARMMEX")
        else:
            c.eye(1000, 950)
            x, z = 560, 375
        if case == "move":
            # Explicit commands use the engine's placement button; in the
            # right-button interface a right click cancels an armed command.
            c.keys(f"pclick:{x},{z}")
        else:
            c.resource_click(x, z)
        if case == "alt":
            c.keys("up:alt")
        orders = c.once(c.orders("ARMCK") if any(o["action"] != 41 for o in c.orders("ARMCK")) else None,
                        f"{case} priority produced no contextual order")
        assert not c.placed(orders), f"{case} priority incorrectly issued a resource build"
    print("resource-priority: explicit Move, Alt, equal owned mex and player toggle off passed")


def refusals(c):
    c.hold()
    c.eye(1250, 1200)
    c.menu(RESOURCE=3, QUICKBUILD=1)
    types = {t["name"].upper(): t["id"] for t in c.call("units", "--json")["units"]}
    defs = c.values("*0x511DE8+0x1439b")[0]
    assert not c.values(f"0x{defs + types['RSNRACK'] * 0x249 + 0x245:x}")[0] & 0x400
    assert not c.values(f"0x{defs + types['RSNGCK'] * 0x249 + 0x245:x}")[0] & 0x20
    c.select("RSNRACK")
    x, z = c.screen("ARMMEX")
    c.resource_click(x, z)
    orders = c.once(c.orders("RSNRACK") if any(o["action"] != 41 for o in c.orders("RSNRACK")) else None,
                    "reclaim-ineligible builder received no ordinary contextual order")
    assert not c.placed(orders) and all(o["action"] for o in orders), "refused reclaim produced a build or action zero"
    c.stop("RSNRACK")
    c.select("ARMACK", "RSNRACK", "RSNGCK", "CORCK")
    # RSNGCK cannot Guard, so its queue holds a Move, and the world is held through the
    # comparison, so no step can complete it. The click issues every order it makes at once
    # (tagpu_resource.c click), and a held world still takes input.
    c.call("order", "move", "pos", "4500", "2500", "--unit", c.units["RSNGCK"], "--expect", "RSNGCK")
    before = c.orders("RSNGCK")
    c.resource_click(x, z)
    c.once(any(o["type"] == 88 for o in c.orders("ARMACK")), "eligible builder did not upgrade")
    assert not c.placed(c.orders("RSNRACK")), "reclaim-ineligible builder became a primary"
    c.once(any(o["target"] == c.address("ARMACK") for o in c.orders("CORCK")),
           "eligible constructor did not receive its helper Guard")
    assert c.orders("RSNGCK") == before, "Guard refusal changed the helper's queue"
    print("resource-refusals: reclaim and Guard refusal, eligible upgrade and helper fallback passed")


def viewer(c, kind="ARMACK"):
    tick, paused = c.values("*0x511DE8+0x38a47", "*0x511DE8+0x38a51:2")
    assert paused & 1, "viewer probe requires the world paused"
    types = {t["name"].upper(): t["id"] for t in c.call("units", "--json")["units"]}
    c.select(kind)
    c.once(c.values("*0x511DE8+0x37e9e:2")[0] == types[kind],
           "native selection did not display the advanced constructor's type")
    assert c.values("*0x511DE8+0x38a47")[0] == tick, "viewer selection advanced the simulation"
    print(f"resource viewer: paused native {kind} selection updated the displayed unit type")


def frontend(name, gamedir):
    """Pre-match native Visuals Undo must preserve the raw preference under a cap."""
    c = Check.__new__(Check)
    c.name = name
    cfg, profile = gamedir / "impure.cfg", gamedir / "profiles/retail.ini"
    old_cfg = cfg.read_bytes() if cfg.exists() else None
    old_profile = profile.read_bytes()
    packet_off = gamedir / "tagpu_packet.off"
    old_packet_off = packet_off.read_bytes() if packet_off.exists() else None
    def controls():
        return {g["name"]: g for g in c.call("ui", "--json")["gadgets"]}
    def stored():
        text = cfg.read_text()
        m = re.search(r"(?m)^resource_radius\.retail=(\w+)", text)
        return m.group(1) if m else None
    try:
        c.call("stop")
        text = (old_cfg or b"").decode()
        text = re.sub(r"(?m)^resource_radius\.retail=[^\n]*\n?", "", text)
        cfg.write_text(text + "\nresource_radius.retail=8\n")
        packet_off.touch()
        c.call("launch", "--defaults", "--keep-dll")
        c.until(lambda: c.call("ui", "--json")["gui"] == "MAINMENU.GUI", "frontend did not reach menu")
        for button in ("SINGLE", "Options", "VISUALS"):
            c.call("ui", "click", button)
        c.until(lambda: controls()["RESOURCE"]["stage"] == 3, "profile cap did not apply")
        assert stored() == "8", "opening Visuals changed the raw preference"
        c.call("ui", "click", "UNDO")
        c.until(lambda: controls()["RESOURCE"]["stage"] == 3, "untouched Undo lost effective cap")
        assert stored() == "8", "untouched Undo overwrote the raw preference"
        profile.write_text("manual_snapping=1\nquick_build=1\ndefault_radius=3\nmax_radius=8\nquick_build_default=1\n")
        c.until(lambda: controls()["RESOURCE"]["stage"] == 8, "untouched Undo destroyed the preference above the cap")
        profile.write_bytes(old_profile)
        c.until(lambda: controls()["RESOURCE"]["stage"] == 3, "original cap did not reload")
        c.call("ui", "click", "RESOURCE")
        c.until(lambda: stored() == "0", "resource click did not commit")
        c.call("ui", "click", "UNDO")
        c.until(lambda: stored() == "8", "Undo did not restore the raw preference")
        profile.write_text("manual_snapping=1\nquick_build=1\ndefault_radius=3\nmax_radius=8\nquick_build_default=1\n")
        c.until(lambda: controls()["RESOURCE"]["stage"] == 8, "raising the cap did not recover the preference")
        print("resource-frontend: untouched/touched Undo and cap reload without packets passed")
    finally:
        try:
            c.call("stop")
        finally:
            profile.write_bytes(old_profile)
            if old_cfg is None:
                cfg.unlink(missing_ok=True)
            else:
                cfg.write_bytes(old_cfg)
            if old_packet_off is None:
                packet_off.unlink(missing_ok=True)
            else:
                packet_off.write_bytes(old_packet_off)


def catalogue(name):
    """Refresh scenario validation from the engine after private content changes."""
    c = Check.__new__(Check)
    c.name = name
    try:
        c.call("stop")
        c.call("launch", "--defaults", "--keep-dll")
        names = {t["name"].upper() for t in c.call("units", "--json")["units"]}
        assert {"RSNRACK", "RSNGCK"} <= names, "private refusal constructors did not load"
        print("resource catalogue: refusal constructors loaded")
    finally:
        c.call("stop")


CHECK_FAILURES = (AssertionError, ValueError, subprocess.TimeoutExpired)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("instance")
    p.add_argument("case", choices=("mex", "upgrade", "queued-upgrade", "plain", "uniform", "mixed", "locked", "inherit", "geo-upgrade", "nearby", "priority", "refusals", "viewer", "frontend", "catalogue"))
    p.add_argument("--gamedir", type=Path)
    p.add_argument("--classic", action="store_true", help="exercise native left-button contextual orders")
    p.add_argument("--viewer-unit", choices=("ARMACK", "RSNRACK"), default="ARMACK")
    a = p.parse_args()
    try:
        if a.case == "catalogue":
            catalogue(a.instance)
            return 0
        if a.case == "frontend":
            assert a.gamedir, "frontend regression requires --gamedir"
            frontend(a.instance, a.gamedir)
            return 0
        run = {"mex": lambda c: mex(c, classic=a.classic), "upgrade": upgrade,
         "queued-upgrade": lambda c: upgrade(c, shift=True),
         "geo-upgrade": geo_upgrade,
         "nearby": nearby,
         "priority": priority,
         "refusals": refusals,
         "viewer": lambda c: viewer(c, a.viewer_unit),
         **{m: (lambda c, m=m: policy(c, m, a.gamedir)) for m in ("locked", "inherit")},
         **{m: (lambda c, m=m: terrain(c, m)) for m in ("plain", "uniform", "mixed")}}[a.case]
        check = Check(a.instance, {a.viewer_unit} if a.case == "viewer" else FIXTURE_UNITS)
        # The case's own failure is the exit reason and is printed first; a cleanup step that
        # fails after it is reported beside it, never in its place. A case that raised anything
        # else leaves the traceback as the reason, and the cleanup failures as the lines above it.
        failures, raised = [], True
        try:
            run(check)
            raised = False
        except CHECK_FAILURES as e:
            failures.append(("", e))
            raised = False
        finally:
            cleanup = [("release", check.release)]   # the world handed back held, as it came
            if a.case in ("locked", "inherit"):
                cleanup.insert(0, ("menu.open=off", lambda: check.call("arm", "menu.open=off")))
            for what, step in cleanup:
                try:
                    step()
                except CHECK_FAILURES as e:
                    failures.append((what, e))
            for i, (what, e) in enumerate(failures):
                if i == 0 and not raised:
                    print(f"resource regression FAILED: {what + ': ' if what else ''}{e}",
                          file=sys.stderr)
                else:
                    print(f"resource regression: {what} failed as well: {e}", file=sys.stderr)
        if failures:
            return 1
    except CHECK_FAILURES as e:
        print(f"resource regression FAILED: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
