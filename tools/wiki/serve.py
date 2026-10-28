#!/usr/bin/env python3
"""serve.py -- the in-game wiki mockup's server: the repository, and the COB host behind the mockup's sim.js.

    python3 tools/wiki/serve.py [--bind ADDR] [--port N]     then http://127.0.0.1:8000/tools/wiki/mockup/
    python3 tools/wiki/serve.py --stdio                       the host alone, one JSON request a line

The repository is served as tools/hudart/serve.py serves it, every response marked `Cache-Control: no-cache`.
`POST /cob` (and each line of --stdio) is one call on the in-game wiki's COB host, tagpu/ddraw/src/tagpu_wikicob.c,
loaded through ctypes from the library tools/tacob builds (the core and the host, compiled on first use into a
gitignored cache): the same C the DLL's wiki runs, so the mockup's units run their scripts exactly as the game's
wiki runs them (research/notes/ingame-wiki-cob-plan.md, *The wiki host*). The page's sim.js calls it with
synchronous requests; tools/wiki/cobhost.mjs gives node's checks the same calls over --stdio.

A request is a JSON object with `op`:
  new     {"types": [{"name", "pack"} | null, ...]} -> {"world": id, "notes": [...], ...state}: a host for the types, in the
          simulation's type order; each named type's script and posed order are read from the gfx pack folder
          `pack` (a path under tools/wiki/mockup/gfx/, which tools/wiki/gfx.py writes: units/<type>.cob and the
          scene.json mesh's `script`), a type without them (null, or none in the pack) runs no script
  spawn   {"world", "type", "facts"} -> {"unit": handle, ...state}
  call    {"world", "unit", "script", "how", "args", "facts"} -> {"r": 1 started, 0 refused, -1 faulted, ...state}
  slice   {"world", "facts"} -> state
  activate {"world", "unit", "on"} -> {"r", ...state}: the state setter 0x48B090(unit, 1, on)
  remove  {"world", "unit"} -> state
  free    {"world"} -> {}
`facts` is a list of [unit, owner, x, z, left, health] rows (tagpu_wikicob.h WIKICOBFACT; `left` a number the
float32 holds exactly). The state every call answers is {"values": {unit: [the 21 values]}, "resting": [units],
"hash": 16 hex digits (tagpu_wikicob_hash from the FNV-1a offset basis)}, every handle from 1 to the last.
An error answers {"error": why}."""
import argparse
import ctypes as C
import functools
import json
import struct
import sys
import threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from importlib.machinery import SourceFileLoader
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GFX = (ROOT / "tools/wiki/mockup/gfx").resolve()
FNV_BASIS = 0xCBF29CE484222325
WORLDS_MAX = 64                     # the oldest world goes when a page never frees its own
VALUES = 21                         # WIKICOB_VALUES
PIECES = 256                        # WIKICOB_PIECES


class Type(C.Structure):
    _fields_ = [("cob", C.c_void_p), ("ncob", C.c_size_t), ("pieces", C.c_int), ("visible", C.c_void_p)]


class Fact(C.Structure):
    _fields_ = [("unit", C.c_int32), ("owner", C.c_int32), ("x", C.c_int32), ("z", C.c_int32),
                ("left", C.c_float), ("health", C.c_int32)]


def library():
    tacob = SourceFileLoader("tacob", str(ROOT / "tools/tacob")).load_module()
    lib = tacob.cobvm_lib()
    lib.tagpu_wikicob_new.restype = C.c_void_p
    lib.tagpu_wikicob_new.argtypes = [C.c_int, C.POINTER(Type), C.c_uint32, C.c_char_p, C.c_size_t]
    lib.tagpu_wikicob_free.argtypes = [C.c_void_p]
    lib.tagpu_wikicob_spawn.argtypes = [C.c_void_p, C.c_int, C.POINTER(Fact), C.c_int]
    lib.tagpu_wikicob_remove.argtypes = [C.c_void_p, C.c_int]
    lib.tagpu_wikicob_activate.argtypes = [C.c_void_p, C.c_int, C.c_int]
    lib.tagpu_wikicob_call.argtypes = [C.c_void_p, C.c_int, C.c_char_p, C.c_int, C.c_int, C.POINTER(C.c_int32),
                                       C.POINTER(Fact), C.c_int]
    lib.tagpu_wikicob_slice.argtypes = [C.c_void_p, C.POINTER(Fact), C.c_int]
    lib.tagpu_wikicob_slice.restype = None
    lib.tagpu_wikicob_value.argtypes = [C.c_void_p, C.c_int, C.c_int]
    lib.tagpu_wikicob_value.restype = C.c_int32
    lib.tagpu_wikicob_resting.argtypes = [C.c_void_p, C.c_int]
    lib.tagpu_wikicob_note.argtypes = [C.c_void_p, C.c_int]
    lib.tagpu_wikicob_note.restype = C.c_char_p
    lib.tagpu_wikicob_hash.argtypes = [C.c_void_p, C.c_uint64]
    lib.tagpu_wikicob_hash.restype = C.c_uint64
    return lib


class Host:
    """Every world the clients made, by id. One lock: the core's calls on one world are one thread's at a time."""

    def __init__(self):
        self.lib = library()
        self.worlds = {}            # id -> (handle, units)
        self.next = 1
        self.lock = threading.Lock()

    def pack_type(self, spec):
        """(cob bytes, visible bytes) of a type from its pack, or None."""
        if not spec:
            return None
        pack = (ROOT / spec["pack"]).resolve()
        if GFX not in pack.parents and pack != GFX:
            raise ValueError(f"{spec['pack']} is not a pack under tools/wiki/mockup/gfx/")
        scene = json.loads((pack / "scene.json").read_text())
        mesh = (scene.get("units") or {}).get("meshes", {}).get(spec["name"])
        script = mesh and mesh.get("script")
        if not script:
            return None
        return (pack / script["file"]).read_bytes(), bytes(script["visible"][:PIECES])

    def new(self, req):
        specs = req["types"]
        keep = [self.pack_type(s) for s in specs]
        arr = (Type * max(1, len(specs)))()
        for i, got in enumerate(keep):
            if got:
                cob, vis = got
                arr[i].cob = C.cast(C.c_char_p(cob), C.c_void_p)
                arr[i].ncob = len(cob)
                arr[i].pieces = len(vis)
                arr[i].visible = C.cast(C.c_char_p(vis), C.c_void_p)
        err = C.create_string_buffer(256)
        h = self.lib.tagpu_wikicob_new(len(specs), arr, req.get("seed", 1), err, 256)
        if not h:
            raise ValueError(err.value.decode("latin-1"))
        wid = self.next
        self.next += 1
        self.worlds[wid] = [h, 0]
        while len(self.worlds) > WORLDS_MAX:
            old = min(self.worlds)
            self.lib.tagpu_wikicob_free(self.worlds.pop(old)[0])
        notes = [self.lib.tagpu_wikicob_note(h, i).decode("latin-1") for i in range(len(specs))]
        return {"world": wid, "notes": notes, **self.state(self.worlds[wid])}

    def facts(self, rows):
        arr = (Fact * max(1, len(rows)))()
        for i, (unit, owner, x, z, left, health) in enumerate(rows):
            arr[i] = Fact(unit, owner, x, z, left, health)
        return arr, len(rows)

    def state(self, w):
        h, n = w
        values = {str(u): [self.lib.tagpu_wikicob_value(h, u, k) for k in range(VALUES)] for u in range(1, n + 1)}
        resting = [u for u in range(1, n + 1) if self.lib.tagpu_wikicob_resting(h, u)]
        return {"values": values, "resting": resting,
                "hash": f"{self.lib.tagpu_wikicob_hash(h, FNV_BASIS):016x}"}

    def call(self, req):
        op = req.get("op")
        with self.lock:
            if op == "new":
                return self.new(req)
            w = self.worlds.get(req.get("world"))
            if w is None:
                raise ValueError(f"no world {req.get('world')}")
            h = w[0]
            if op == "free":
                self.lib.tagpu_wikicob_free(self.worlds.pop(req["world"])[0])
                return {}
            if op == "spawn":
                f, n = self.facts(req["facts"])
                u = self.lib.tagpu_wikicob_spawn(h, req["type"], f, n)
                w[1] = max(w[1], u)
                return {"unit": u, **self.state(w)}
            if op == "call":
                f, n = self.facts(req["facts"])
                args = req.get("args") or []
                a = (C.c_int32 * 4)(*args[:4])
                r = self.lib.tagpu_wikicob_call(h, req["unit"], req["script"].encode("latin-1"), req["how"],
                                                len(args), a, f, n)
                return {"r": r, **self.state(w)}
            if op == "slice":
                f, n = self.facts(req["facts"])
                self.lib.tagpu_wikicob_slice(h, f, n)
                return self.state(w)
            if op == "activate":
                return {"r": self.lib.tagpu_wikicob_activate(h, req["unit"], req["on"]), **self.state(w)}
            if op == "remove":
                self.lib.tagpu_wikicob_remove(h, req["unit"])
                return self.state(w)
            raise ValueError(f"no op {op!r}")

    def answer(self, line):
        try:
            return self.call(json.loads(line))
        except (ValueError, KeyError, TypeError, OSError, struct.error) as e:
            return {"error": str(e)}


class Handler(SimpleHTTPRequestHandler):
    host = None

    def end_headers(self):
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def do_POST(self):
        if self.path != "/cob":
            self.send_error(404)
            return
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        out = json.dumps(self.host.answer(body)).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def log_message(self, fmt, *args):
        if not self.path.startswith("/cob"):
            super().log_message(fmt, *args)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--stdio", action="store_true", help="answer one JSON request a line on stdin")
    a = ap.parse_args()
    host = Host()
    if a.stdio:
        for line in sys.stdin:
            if line.strip():
                sys.stdout.write(json.dumps(host.answer(line)) + "\n")
                sys.stdout.flush()
        return
    Handler.host = host
    ThreadingHTTPServer((a.bind, a.port), functools.partial(Handler, directory=str(ROOT))).serve_forever()


if __name__ == "__main__":
    main()
