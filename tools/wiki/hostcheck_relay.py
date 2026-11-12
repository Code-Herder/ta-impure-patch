#!/usr/bin/env python3
"""hostcheck_relay.py -- tools/wiki/hostcheck.mjs's half for serve.py: the simulation relay (SimRelay) and POST /sim
held to their promises, offline. Each line printed is `ok  ` or `FAIL` and a case; exit 1 when one failed.

  the relay's refusals: an empty or blank body, one holding a NUL or a newline, each answered before the lock is
    taken (the host is not even started); a body past 16 MB or with no length refused by the server unread;
  the packs' root: a pack folder outside tools/wiki/mockup/gfx/ refused by the host, whether named plainly, through
    a JSON escape the relay forwards as sent, or through a link inside the gfx tree;
  a host killed mid-answer: the relay answers the exit, never the part of the answer it read, and the next request
    meets a new host, which refuses a call stamped for the old one.
The kill waits for a CONDITION, the host blocked writing an answer larger than its pipe, never for a time."""
import http.client
import json
import os
import shutil
import signal
import sys
import threading
import time
from http.server import ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/wiki"))
import serve  # noqa: E402

failed = 0


def check(ok, what, why=""):
    global failed
    failed += not ok
    print(f"{'ok  ' if ok else 'FAIL'} relay: {what}{'' if ok or not why else f': {why}'}", flush=True)


def blocked_writing(pid):
    try:
        return "pipe_write" in Path(f"/proc/{pid}/wchan").read_text()
    except OSError:
        return False


# a batch whose answer is many times the 64 KB pipe: the host blocks on it until the answer is read
BIG_ANSWER = json.dumps({"op": "batch", "calls": [{"op": "hello"}] * 400}).encode()


class KillMidAnswer:
    """The host's stdout as the relay reads it, whose first readline kills the host once it is blocked writing the
    answer, and then reads what the pipe holds"""

    def __init__(self, proc):
        self.proc, self.real, self.blocked = proc, proc.stdout, False

    def readline(self):
        for _ in range(6000):
            if blocked_writing(self.proc.pid):
                self.blocked = True
                break
            time.sleep(0.005)
        os.kill(self.proc.pid, signal.SIGKILL)
        return self.real.readline()


def refusals():
    r = serve.SimRelay()
    for body, what in ((b"", "an empty body"), (b"  \r\n", "a blank body"), (b'{"op":"hel\0lo"}', "a body holding a NUL"),
                       (b'{"op":\n"hello"}', "a body of two lines")):
        a = json.loads(r.answer(body))
        check("error" in a and r.proc is None, f"{what} is refused before the host is asked", a)
    a = json.loads(r.answer(b'  {"op":"hello"}\r\n'))
    check(a.get("version") == 1 and len(a.get("instance", "")) == 32, "a body with whitespace around it is answered", a)


def http_limits():
    serve.Handler.sim = serve.SimRelay()
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), serve.QuietHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        for length, want in ((str(serve.LINE_MAX + 1), 413), ("nope", 400), ("-5", 400)):
            c = http.client.HTTPConnection("127.0.0.1", httpd.server_address[1])
            c.putrequest("POST", "/sim")
            c.putheader("Content-Type", "application/json")
            c.putheader("Content-Length", length)
            c.endheaders()
            st = c.getresponse().status
            c.close()
            check(st == want, f"a body of Content-Length {length} is refused unread with {want}", st)
        c = http.client.HTTPConnection("127.0.0.1", httpd.server_address[1])
        c.request("POST", "/sim", body=b'{"op":"fits","w":40,"h":30}', headers={"Content-Type": "application/json"})
        a = json.loads(c.getresponse().read())
        check("fits" in a, "a request through /sim is answered", a)
    finally:
        httpd.shutdown()


def packs_root():
    out = ROOT / "build/hostcheck-escape"
    link = ROOT / "tools/wiki/mockup/gfx/hostcheck-link"
    out.mkdir(parents=True, exist_ok=True)
    (out / "scene.json").write_text(json.dumps({"units": {"meshes": {"ARMCOM": {
        "script": {"file": "x.cob", "visible": [1]}, "sound": {"cat": 1, "count": [1]}}}}}))
    (out / "x.cob").write_bytes(b"\0" * 64)
    made_gfx = not link.parent.exists()
    link.parent.mkdir(parents=True, exist_ok=True)
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(Path(os.path.relpath(out, link.parent)))
    r = serve.SimRelay()
    try:
        for body, what in ((b'{"op":"packs","dirs":["build/hostcheck-escape"]}', "a pack outside the gfx tree"),
                           (b'{"op":"p\\u0061cks","dirs":["build/hostcheck\\u002descape"]}', "the same spelled with JSON escapes"),
                           (b'{"op":"packs","dirs":["tools/wiki/mockup/gfx/hostcheck-link"]}', "a link in the gfx tree to it"),
                           (b'{"op":"batch","calls":[{"op":"packs","dirs":["build/hostcheck-escape"]}]}', "the same in a batch")):
            a = json.loads(r.answer(body))
            a = a["answers"][0] if "answers" in a else a
            check("outside the packs' root" in a.get("error", ""), f"{what} is refused by the host", a)
        a = json.loads(r.answer(b'{"op":"packs","dirs":["tools/wiki/mockup/gfx/no-such-pack"]}'))
        check("packs" in a, "an absent pack under the gfx tree holds nothing and is answered", a)
    finally:
        link.unlink()
        if made_gfx:
            shutil.rmtree(link.parent, ignore_errors=True)
        shutil.rmtree(out, ignore_errors=True)


def kill_mid_answer():
    r = serve.SimRelay()
    old = json.loads(r.answer(b'{"op":"hello"}'))["instance"]
    k = KillMidAnswer(r.proc)
    r.proc.stdout = k
    a = r.answer(BIG_ANSWER)
    check(k.blocked, "the host blocks writing an answer larger than its pipe")
    try:
        got = json.loads(a)
    except ValueError:
        got = {"error": f"no JSON: {a[:80]!r}..."}
    check(got.get("error", "").startswith("the host exited"), "a host killed mid-answer is answered as its exit", got)
    a = json.loads(r.answer(json.dumps({"op": "fits", "w": 40, "h": 30, "in": old}).encode()))
    check("error" in a and a.get("host") not in (None, old), "the next host refuses a call stamped for the old one", a)
    new = json.loads(r.answer(b'{"op":"hello"}'))["instance"]
    check(new != old and a.get("host") == new, "and names itself, a new instance", (old, new))


def stdio():
    import subprocess
    p = subprocess.run([sys.executable, str(ROOT / "tools/wiki/serve.py"), "--stdio"], cwd=ROOT,
                       input=b'\n{"op":"fits","w":40,"h":30}\n   \n', capture_output=True, timeout=600)
    out = p.stdout.decode().splitlines()
    check(len(out) == 3 and "error" in json.loads(out[0]) and "fits" in json.loads(out[1]),
          "--stdio answers every line, an empty one included", out)


refusals()
http_limits()
packs_root()
kill_mid_answer()
stdio()
sys.exit(1 if failed else 0)
