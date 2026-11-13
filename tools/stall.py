"""stall.py -- the bound of a wait on a running level, kept off the wait's path.

A wait on a running level returns on a line of the game's log (tareplay's Log.wait_running,
resource_regression's Check._taken), and a loaded machine only makes that line later. What bounds
such a wait is the game's own progress: GameTime standing still for `stall_s` is a hung or dead
game. That is all a Stall decides -- the game could not run -- and never an outcome: the wait
returns the first line of its own that matches, and nothing here reads, makes or withholds one.

WHEN IT LOOKS. A Stall asks nothing for the first EVERY seconds of its wait: a wait begins on a
level the harness has just armed or resumed, and most lines come within that. From then on it
keeps one GameTime read in flight, asked EVERY after the last answer: it writes the peek trigger
itself (tagpu_peek.h) and reads the answer from the log at the wait's next looks, so asking costs
one file write and the wait goes on looking meanwhile. A `tacli peek` process the wait blocked
on cost 0.4 s of Python start alone, and asked at the start of every wait it was about 1.3 s of a
6.45 s calibration run (MEASURED 2026-11-13: 8 850 runs of 14 GDI lanes, 2.65 peeks a run).

A read the game has not answered is no progress, not a failure: a seek's rebuild holds the game
thread inside one frame (MEASURED 2026-10-12, the battle's X lane at a load average of 63-87: two
seeks of 6.5 s wall each, back to back), so stall_s alone decides when that is a hung game.

THE BOUND. GameTime that stops moving at a moment t is named no later than t + EVERY + one answer
+ stall_s: the read asked at most EVERY after t sees the stopped value, the game answers it at its
next peek visit (every 15th flip, tagpu_peek_frame), and stall_s runs from that answer. A read
never answered counts from when it was asked, so a level that never answers is named at most
EVERY + stall_s after the wait began.

ONE READ OF ITS OWN AT A TIME, AND NONE LEFT BEHIND. The peek trigger is one file, which the DLL
reads, closes and then deletes by name (tagpu_peek_frame), so a request written between its read
and its delete is deleted unread. Every writer of it awaits its own answer before anyone writes the
next one (tacli's _peek, engrun's Lane.peek), and the DLL logs an answer only after that delete.
A Stall keeps the same order: it asks again only once answered, and a read still in flight when
the wait's line comes is retired before the wait hands the game on (retire): the log is read until
the answer, or until the game is gone, and a request the DLL can no longer read is removed. That
holds a return only for a read asked in the last moments before the line, which a wait shorter
than EVERY never has, and for no longer than the game takes to answer it.

ITS ANSWERS ARE ITS OWN. Each read is spelled with a nonce, the process's id and a counter added to
GameTime's address and subtracted again by a third term: the DLL adds the terms modulo 2^32
(tagpu_peek.c, eval: `unsigned int addr; addr += off`) and logs the spec as written, so the line
answers this read alone. A late answer is never another peek's, and another's never this one's
(tacli's _peek_rows matches a line by its spec)."""
import itertools
import os
import re
import time
from pathlib import Path

import talog

GAMETIME = "*0x511DE8+0x38A47"      # main+0x38A47, the level's tick count (u32)
PEEK_TRIGGER = "tagpu_peek.trigger"
# A temporary name tacli's launch sweeps with its own (`tagpu_*.tacli-tmp`, tacli _launch), so a
# write cut short leaves nothing the next game reads.
TMP_SUFFIX = ".tacli-tmp"
_NONCE = itertools.count(1)
VALUE_RX = re.compile(r"(\d+)")


class Stall:
    EVERY = 5.0         # s: the quiet start of a wait, and the time between two reads

    def __init__(self, gamedir, stall_s, fail, gone=lambda: False, nap=time.sleep, look_s=0.02):
        """`fail(text)` is the exception a stall raises; `gone()` says the game can answer
        nothing more (crashed or exited), which ends a retire; `nap` sleeps between the looks of
        a retire, `look_s` apart."""
        self.gamedir = Path(gamedir)
        self.stall_s, self.fail, self.gone, self.nap, self.look_s = stall_s, fail, gone, nap, look_s
        self.g = None           # the last GameTime read
        self.since = None       # when GameTime was last seen to move, or the first read asked
        self.start = time.monotonic()
        self.next = None        # when the next read is asked; set at the first look, from start
        self.asked = None       # the spec of the read in flight
        self.cursor = None
        self.part = ""

    def check(self):
        """One look, which never waits: the GameTime a read answered since the last look, or
        None. Asks the next read when it is due, and raises fail() once GameTime has stood
        still for stall_s."""
        now = time.monotonic()
        if self.next is None:
            self.next = self.start + self.EVERY
        g = self._answer() if self.asked else None
        if g is not None:
            self._moved(g, now)
        if self.asked is None and now >= self.next:
            self._ask()
            if self.since is None:
                self.since = now
        self._bound(now)
        return g

    def saw(self, g):
        """A GameTime the caller read itself, for a loop that already reads it every look: it
        bounds that loop as check() does, and asks nothing."""
        now = time.monotonic()
        if self.since is None:
            self.since = now
        self._moved(g, now)
        self._bound(now)

    def retire(self):
        """The read in flight, if any, answered before the wait hands the game on, or dropped once
        the game can answer nothing more (ONE READ OF ITS OWN AT A TIME)."""
        while self.asked:
            if self.gone():
                (self.gamedir / PEEK_TRIGGER).unlink(missing_ok=True)
                self.asked = None
                return
            self.check()
            if self.asked:
                self.nap(self.look_s)

    def _moved(self, g, now):
        if g != self.g:
            self.g, self.since = g, now

    def _bound(self, now):
        if self.since is not None and now - self.since > self.stall_s:
            raise self.fail(f"the level stood at GameTime {self.g} for {self.stall_s} s while it ran"
                            + (", its last peek unanswered" if self.asked else ""))

    def _ask(self):
        pid, n = os.getpid(), next(_NONCE)
        self.asked = f"{GAMETIME}+0x{pid:X}+0x{n:X}+0x{-(pid + n) & 0xFFFFFFFF:X}:4"
        self.cursor = talog.Cursor(self.gamedir)    # before the write: the answer comes after it
        self.part = ""
        trigger = self.gamedir / PEEK_TRIGGER
        tmp = trigger.with_name(f"{trigger.name}.{pid}{TMP_SUFFIX}")
        tmp.write_text(self.asked + "\n")
        os.replace(tmp, trigger)                    # whole: the DLL never reads half a request

    def _answer(self):
        """The read's GameTime once its line is logged, which ends the read; None before that."""
        lines = (self.part + self.cursor.text(advance=True)).split("\n")
        self.part = lines.pop()
        head = f"peek: {self.asked} "
        for ln in lines:
            if ln.startswith(head):
                self.asked = None
                self.next = time.monotonic() + self.EVERY
                m = VALUE_RX.match(ln.rpartition(" = ")[2])
                # an unreadable GameTime answers the read and shows no progress
                return int(m.group(1)) if m else None
        return None
