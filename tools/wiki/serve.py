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
  new     {"types": [{"name", "pack"} | null, ...]} -> {"world": id, "tops": [...], ...state}: a host for the types,
          in the simulation's type order; each named type's script, posed order and model shape are read from the gfx
          pack folder `pack` (a path under tools/wiki/mockup/gfx/, which tools/wiki/gfx.py writes: units/<type>.cob
          and the scene.json mesh's `script`), a type without them (null, or none in the pack) runs no script; `tops`
          is each type's model height (tagpu_wikicob_top), 0 for one without
  spawn   {"world", "type", "facts"} -> {"unit": handle, ...state}
  call    {"world", "unit", "script", "how", "args", "facts"} -> {"r": 1 started, 0 refused, -1 faulted, ...state}
  slice   {"world", "facts"} -> state
  activate {"world", "unit", "on"} -> {"r", ...state}: the state setter 0x48B090(unit, 1, on)
  query   {"world", "unit", "script", "io", "facts"} -> {"r", "io", ...state}: a query script run inside the call
          (tagpu_wikicob_query), `io` its four words in and out
  origin  {"world", "unit", "piece"} -> {"r", "turned", "base"}: the piece's origin (tagpu_wikicob_origin); it
          changes nothing, so it answers no state
  remove  {"world", "unit"} -> state
  models  {"world"} -> {"models": {unit: base64}}: each unit's posed model block as tagpu_wikicob_model gives it
          (its PrimitiveStructs in the posed order), a unit that runs no script left out; it changes nothing, so it
          answers no state. The page asks once a drawn frame after a call (cob.js `model`).
  free    {"world"} -> {}
`facts` is a list of [unit, owner, x, z, left, health] rows (tagpu_wikicob.h WIKICOBFACT; `left` a number the
float32 holds exactly). The state every call answers is {"values": {unit: [the 21 values]}, "resting": [units],
"notes": [each type's quarantine note, "" for none], "hash": 16 hex digits (tagpu_wikicob_hash from the FNV-1a
offset basis)}, every handle from 1 to the last.
An error answers {"error": why}."""
import argparse
import base64
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
PRIM_BASE, PRIM_BYTES = 0x22, 0x36  # TAGPU_COBVM_PRIM_BASE, _PRIM_BYTES: TAGPU_COBVM_MODEL_BYTES(n)


class Type(C.Structure):
    _fields_ = [("cob", C.c_void_p), ("ncob", C.c_size_t), ("pieces", C.c_int), ("visible", C.c_void_p),
                ("offset", C.c_void_p), ("parent", C.c_void_p), ("top", C.c_int32)]


class Fact(C.Structure):
    _fields_ = [("unit", C.c_int32), ("owner", C.c_int32), ("x", C.c_int32), ("z", C.c_int32),
                ("left", C.c_float), ("health", C.c_int32)]


def library():
    tacob = SourceFileLoader("tacob", str(ROOT / "tools/tacob")).load_module()
    return bind(tacob.cobvm_lib())


def bind(lib):
    """`lib`, a build of tagpu_wikicob.c with the core, with the host's signatures set (tools/test_cobvm.py binds its
    sanitizer build here too)."""
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
    lib.tagpu_wikicob_model.argtypes = [C.c_void_p, C.c_int, C.POINTER(C.c_int)]
    lib.tagpu_wikicob_model.restype = C.c_void_p
    lib.tagpu_wikicob_query.argtypes = [C.c_void_p, C.c_int, C.c_char_p, C.POINTER(C.c_int32), C.POINTER(Fact),
                                        C.c_int]
    lib.tagpu_wikicob_origin.argtypes = [C.c_void_p, C.c_int, C.c_int, C.POINTER(C.c_int32), C.POINTER(C.c_int32)]
    lib.tagpu_wikicob_top.argtypes = [C.c_void_p, C.c_int]
    lib.tagpu_wikicob_top.restype = C.c_int32
    return lib


def shape(script):
    """The model shape of a pack's `script` (gfx.py `model_shape`) as the host takes it (WIKICOBTYPE): the offsets'
    words, the parents and the height, each as bytes the host borrows; empty offsets and parents for a pack written
    before the shape was, which leave every piece at the model's own point."""
    off = script.get("offset") or []
    par = script.get("parent") or []
    words = [w for o in off for w in o]
    return (struct.pack(f"<{len(words)}i", *words) if len(off) == len(script["visible"]) else b"",
            struct.pack(f"<{len(par)}h", *par) if len(par) == len(script["visible"]) else b"",
            int(script.get("top", 0)))


class Host:
    """Every world the clients made, by id. One lock: the core's calls on one world are one thread's at a time."""

    def __init__(self):
        self.lib = library()
        self.worlds = {}            # id -> [handle, the last unit's handle, the type count]
        self.next = 1
        self.lock = threading.Lock()

    def pack_type(self, spec):
        """(cob bytes, visible bytes, shape) of a type from its pack, or None: None too for a model past PIECES, which
        the DLL's 3D load refuses, so its type has no script there either."""
        if not spec:
            return None
        pack = (ROOT / spec["pack"]).resolve()
        if GFX not in pack.parents and pack != GFX:
            raise ValueError(f"{spec['pack']} is not a pack under tools/wiki/mockup/gfx/")
        scene = json.loads((pack / "scene.json").read_text())
        mesh = (scene.get("units") or {}).get("meshes", {}).get(spec["name"])
        script = mesh and mesh.get("script")
        if not script or len(script["visible"]) > PIECES:
            return None
        return (pack / script["file"]).read_bytes(), bytes(script["visible"]), shape(script)

    def new(self, req):
        specs = req["types"]
        keep = [self.pack_type(s) for s in specs]
        arr = (Type * max(1, len(specs)))()
        for i, got in enumerate(keep):
            if got:
                cob, vis, (off, par, top) = got
                arr[i].cob = C.cast(C.c_char_p(cob), C.c_void_p)
                arr[i].ncob = len(cob)
                arr[i].pieces = len(vis)
                arr[i].visible = C.cast(C.c_char_p(vis), C.c_void_p)
                arr[i].offset = C.cast(C.c_char_p(off), C.c_void_p) if off else None
                arr[i].parent = C.cast(C.c_char_p(par), C.c_void_p) if par else None
                arr[i].top = top
        err = C.create_string_buffer(256)
        h = self.lib.tagpu_wikicob_new(len(specs), arr, req.get("seed", 1), err, 256)
        if not h:
            raise ValueError(err.value.decode("latin-1"))
        wid = self.next
        self.next += 1
        self.worlds[wid] = [h, 0, len(specs)]       # handle, the last unit's handle, the type count
        while len(self.worlds) > WORLDS_MAX:
            old = min(self.worlds)
            self.lib.tagpu_wikicob_free(self.worlds.pop(old)[0])
        tops = [self.lib.tagpu_wikicob_top(h, i) for i in range(len(specs))]
        return {"world": wid, "tops": tops, **self.state(self.worlds[wid])}

    def facts(self, rows):
        arr = (Fact * max(1, len(rows)))()
        for i, (unit, owner, x, z, left, health) in enumerate(rows):
            arr[i] = Fact(unit, owner, x, z, left, health)
        return arr, len(rows)

    def state(self, w):
        h, n, ntypes = w
        values = {str(u): [self.lib.tagpu_wikicob_value(h, u, k) for k in range(VALUES)] for u in range(1, n + 1)}
        resting = [u for u in range(1, n + 1) if self.lib.tagpu_wikicob_resting(h, u)]
        notes = [self.lib.tagpu_wikicob_note(h, i).decode("latin-1") for i in range(ntypes)]
        return {"values": values, "resting": resting, "notes": notes,
                "hash": f"{self.lib.tagpu_wikicob_hash(h, FNV_BASIS):016x}"}

    def models(self, w):
        """Every unit's posed model block, base64, by handle; the block is the host's until its next call, so it is
        copied out under the lock."""
        h, n, _ntypes = w
        out = {}
        for u in range(1, n + 1):
            k = C.c_int(0)
            at = self.lib.tagpu_wikicob_model(h, u, C.byref(k))
            if at and 0 < k.value <= PIECES:
                out[str(u)] = base64.b64encode(C.string_at(at, PRIM_BASE + k.value * PRIM_BYTES)).decode("ascii")
        return out

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
            if op == "query":
                f, n = self.facts(req["facts"])
                io = (C.c_int32 * 4)(*(req.get("io") or [0, 0, 0, 0])[:4])
                r = self.lib.tagpu_wikicob_query(h, req["unit"], req["script"].encode("latin-1"), io, f, n)
                return {"r": r, "io": list(io), **self.state(w)}
            if op == "origin":
                turned, base = (C.c_int32 * 3)(), (C.c_int32 * 3)()
                r = self.lib.tagpu_wikicob_origin(h, req["unit"], req["piece"], turned, base)
                return {"r": r, "turned": list(turned), "base": list(base)}
            if op == "remove":
                self.lib.tagpu_wikicob_remove(h, req["unit"])
                return self.state(w)
            if op == "models":
                return {"models": self.models(w)}
            raise ValueError(f"no op {op!r}")

    def answer(self, line):
        try:
            return self.call(json.loads(line))
        except (ValueError, KeyError, TypeError, OSError, struct.error, C.ArgumentError) as e:
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


class QuietHandler(Handler):
    def log_message(self, fmt, *args):
        pass


def server(bind="127.0.0.1", port=0, quiet=False):
    """The mockup's server -- the tree's files and /cob -- answering on a daemon thread, for a tool that drives the
    page with the units' scripts (tools/wiki/stage3dcheck.py); port 0 takes a free one."""
    Handler.host = Host()
    httpd = ThreadingHTTPServer((bind, port), functools.partial(QuietHandler if quiet else Handler, directory=str(ROOT)))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


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
