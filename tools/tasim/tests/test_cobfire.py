"""THE SCRIPTED RELOAD, tasim_cobfire.c: a weapon slot's interval between shots under AutoAim's aim and fire protocol,
on scripts compiled here by tacob, so no game file is read. The probe, the COB VM and the validator are compiled
natively under UBSan and AddressSanitizer's sibling checks the VM's own tests run (tools/test_cobvm.py) and called
through ctypes: the unit's own script decides the interval, and nothing it does can take the answer below the declared
reload or past the probe's span."""
import ctypes as C
import importlib.machinery
import importlib.util
import subprocess
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
DDRAW = ROOT / "tagpu" / "ddraw"
SPAN = 900                       # TASIM_FIRE_SPAN


def load_tacob():
    loader = importlib.machinery.SourceFileLoader("tacob", str(ROOT / "tools" / "tacob"))
    spec = importlib.util.spec_from_loader("tacob", loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


tacob = load_tacob()


@pytest.fixture(scope="module")
def lib():
    with tempfile.TemporaryDirectory(prefix="cobfire-") as tmp:
        so = Path(tmp) / "libcobfire.so"
        subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-g",
                        "-fsanitize=undefined", "-fno-sanitize-recover=all", "-I", str(DDRAW / "inc"),
                        "-I", str(DDRAW / "src"), str(DDRAW / "src" / "tasim_cobfire.c"),
                        str(DDRAW / "src" / "tagpu_cobvm.c"), str(DDRAW / "src" / "tagpu_cob_safe.c"),
                        "-o", str(so)], check=True)
        lib = C.CDLL(str(so))
        lib.tagpu_cob_program.restype = C.c_void_p
        lib.tagpu_cob_program.argtypes = [C.c_void_p, C.c_size_t, C.c_char_p, C.c_size_t]
        lib.tagpu_cob_program_free.argtypes = [C.c_void_p]
        lib.tagpu_cob_bind_model.argtypes = [C.c_void_p, C.c_uint32, C.c_char_p, C.c_size_t]
        lib.tasim_cobfire_work.restype = C.c_size_t
        lib.tasim_cobfire_work.argtypes = [C.c_void_p]
        lib.tasim_cobfire_interval.restype = C.c_uint32
        lib.tasim_cobfire_interval.argtypes = [C.c_void_p, C.c_uint, C.c_uint32, C.c_uint32, C.c_void_p, C.c_size_t]
        yield lib


def interval(lib, source, slot, reload, max_reload=None, pieces=None, short=0):
    """slot's interval for a unit whose script is `source` (its pieces bound to `pieces` model nodes, all by
    default), the work `short` bytes short of what the probe asks"""
    cob = tacob.compile_bos(source, "unit.bos")
    blob = tacob.write_cob(cob)
    buf = C.create_string_buffer(blob, len(blob))
    why = C.create_string_buffer(256)
    prog = lib.tagpu_cob_program(buf, len(blob), why, 256)
    assert prog, why.value
    try:
        assert lib.tagpu_cob_bind_model(prog, len(cob.pieces) if pieces is None else pieces, why, 256), why.value
        n = lib.tasim_cobfire_work(prog)
        work = (C.c_ubyte * n)()
        return lib.tasim_cobfire_interval(prog, slot, reload, reload if max_reload is None else max_reload,
                                          work, n - short)
    finally:
        lib.tagpu_cob_program_free(prog)


PLAIN = """
piece base, turret, barrel;
Create() { }
AimPrimary(heading, pitch) { turn turret to y-axis heading speed <300>; wait-for-turn turret around y-axis;
                             return (1); }
FirePrimary() { move barrel to z-axis [-2] now; move barrel to z-axis [0] speed [4]; }
"""

# Escalation's shape: the fire raises a flag and holds it for a sleep, the aim waits for it to fall
BUSY = """
piece base, turret;
static-var busy, maxreload;
Create() { busy = 0; }
SetMaxReloadTime(t) { maxreload = t; }
AimPrimary(heading, pitch) { while (busy) { sleep 150; } return (1); }
FirePrimary() { busy = 1; sleep 150; sleep %d; busy = 0; }
AimSecondary(heading, pitch) { return (1); }
FireSecondary() { }
"""


def test_a_plain_turret_fires_on_its_reload(lib):
    assert interval(lib, PLAIN, 0, 12) == 12
    # never on consecutive ticks: the shot clears the aim, the next tick's AutoAim restarts it and its return comes
    # in that tick's scripts, so the shot after it is a tick later still -- and a wait-for-turn takes one more
    instant = PLAIN.replace("turn turret to y-axis heading speed <300>; wait-for-turn turret around y-axis;", "")
    assert interval(lib, instant, 0, 1) == 2
    assert interval(lib, PLAIN, 0, 1) == 3


def test_a_busy_flag_holds_the_shot_past_the_reload(lib):
    got = interval(lib, BUSY % 1000, 0, 3)
    # the fire's 150 + 1000 ms are 35 ticks at 30 a second, the aim's poll rounds them up to its 150 ms steps
    # (4.5 ticks), and the shot follows the aim's return by a tick: never below the script's hold
    assert 35 <= got <= 35 + 5 + 1
    assert interval(lib, BUSY % 2250, 0, 3) > got
    # a reload longer than the hold is the interval
    assert interval(lib, BUSY % 1000, 0, 90) == 90


def test_each_slot_is_its_own_scripts(lib):
    assert interval(lib, BUSY % 1000, 1, 7) == 7          # Secondary: no flag in its scripts
    assert interval(lib, BUSY % 1000, 0, 7) > 7


def test_what_the_probe_cannot_rate_keeps_the_reload(lib):
    never = PLAIN.replace("return (1);", "return (0);")
    assert interval(lib, never, 0, 20) == 20               # the aim never returns non-zero
    assert interval(lib, PLAIN, 1, 20) == 20               # no AimSecondary: no aim, the reload
    assert interval(lib, PLAIN, 3, 20) == 20               # past the three slots the engine names
    spin = "piece base;\nCreate() { }\nAimPrimary(h, p) { while (1) { } return (1); }\nFirePrimary() { }\n"
    assert interval(lib, spin, 0, 20) == 20                # the VM refuses a thread that never blocks
    assert interval(lib, BUSY % 1000, 0, 3, short=4) == 3  # work short of the probe's blocks
    # a hold past the span leaves fewer than three shots
    assert interval(lib, BUSY % 20000, 0, 3) == 3


def test_set_max_reload_reaches_the_script(lib):
    # the script times itself by SetMaxReloadTime's argument, the slots' largest reload in ms
    src = """
piece base;
static-var busy, maxreload;
Create() { busy = 0; }
SetMaxReloadTime(t) { maxreload = t; }
AimPrimary(heading, pitch) { while (busy) { sleep 30; } return (1); }
FirePrimary() { busy = 1; sleep maxreload * 2; busy = 0; }
"""
    short, long = interval(lib, src, 0, 3, max_reload=15), interval(lib, src, 0, 3, max_reload=30)
    assert 30 <= short <= 33 and 60 <= long <= 63
