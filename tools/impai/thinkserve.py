#!/usr/bin/env python3
"""thinkserve.py -- the thought viewer as a local app: the library's recordings, built when they are opened.

    tools/impai/thinkview.py serve [--port P] [--library DIR]

Serves the viewer's page (thinkview.html, the one the export writes) on 127.0.0.1 ALONE and prints its address;
it opens no browser and no window. The page's first screen is THE LIBRARY (thinklib.py): a group a label, the
newest first, and a group `played · <instance>` for each instance of the main checkout that holds recordings,
which are listed where they lie and never copied. A recording that is BEING RECORDED (thinklib.being_recorded) is
shown as such and is not opened.

WHAT IT ANSWERS, GET alone, and only to a request whose Host is this server's own address:
    /                               the page, in its app mode (its list of games is `null`)
    /api/library                    {"groups": [{"kind": "label" | "played", "name", "title", "newest", "bytes",
                                    "recs": [a row a recording]}]}: the folders as they are now (no index is kept)
    /api/open?k=KIND&g=GROUP&r=NAME the build of that recording: {"state": "queued" | "building" | "ready" |
                                    "failed", "what": the step, "frac": how far through it, "ahead": the builds
                                    before it, "error", "game": THE LIST's row of it once ready}; `again=1` asks a
                                    failed build to be made again
    /cache/<tool>/<key>/<file>      a built file; /cache/<tool>/maps/<file> a map's picture
A group and a recording are LOOKED UP among those the listing found, by name: no part of a request is ever joined
into a path, and a cache file's three components are each held to their alphabet (SAFE) and resolved under the
cache.

BUILT ON DEMAND, CACHED. Opening a recording builds what the export would write for it -- a data file a seat,
THE DENSE TRACKS every thinkview.DENSE_K ticks, the map's picture -- through thinkview.build_game, in ONE worker
thread, a build at a time, while the server goes on answering; the page shows the step and how far it is. THE
CACHE is `<library>/.cache/<tool>/<key>/`: <tool> the digest of the sources a build is a function of
(tool_digest), <key> the digest of the recording's bytes, so a second opening is the files' reading and an entry
is never read by another version of the tools: at its start the server removes every other version's. An entry
is whole when its `game.json` is there, which is written last, into a folder renamed into place. A map's picture
is one file a map (`maps/`, by the map file's CRC), whatever the games played on it. A recording whose file
changes while it is read or built is refused as being recorded.

THE LAYOUT, THE ARCHIVES AND THE NAMES are the recording's own without anyone naming them (thinkview.py:
with_tree_at, where_played, archive_dirs): the source tree at the commit its header names, extracted from the
repository; the game directory that holds the archives its header lists; the content file whose count is its
definitions'. What could not be had is said in the page, never guessed.

Everything derived from the game's archives (a picture, a mod's type names, the players' colours) is written
under the library's `.cache/` and nowhere else."""
import argparse
import hashlib
import json
import os
import queue
import re
import shutil
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import think  # noqa: E402
import thinklib  # noqa: E402
import thinkview  # noqa: E402

HOST = "127.0.0.1"
SAFE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")        # a cache path's component
TYPES = {".js": "text/javascript; charset=utf-8", ".webp": "image/webp", ".json": "application/json"}
MAPS = "maps"
WHOLE = "game.json"             # an entry's last file: THE LIST's row of its game


def tool_digest() -> str:
    """The digest of what a build is a function of beside the recording's bytes: the reader's sources
    (think.reader_digest), the viewer's own that make the data, and the dense tracks' K."""
    h = hashlib.sha256(think.reader_digest().encode() + f"dense={thinkview.DENSE_K}".encode())
    for name in ("thinkview.py", "thinkmap.py", "thinklayout.py"):
        try:
            h.update((HERE / name).read_bytes())
        except OSError:
            h.update(name.encode())
    return h.hexdigest()[:12]


class Builder(threading.Thread):
    """BUILT ON DEMAND, CACHED: the one worker, and what each build has come to."""

    def __init__(self, cache: Path):
        super().__init__(daemon=True, name="thinkview-build")
        self.tool = tool_digest()
        self.root = Path(cache) / self.tool
        self.todo = queue.Queue()
        self.jobs = {}              # {key: {"state", "what", "frac", "error"}}, each replaced whole, never edited
        self.order = []             # the keys queued or building, in the order they were asked for
        self.hashes = {}            # {(dev, ino, size, mtime_ns): key}
        self.lock = threading.Lock()
        self.lays = self.builds = None
        self.pictures = {}
        for meta in sorted((self.root / MAPS).glob("*.json")) if (self.root / MAPS).is_dir() else []:
            try:
                entry = json.loads(meta.read_text())
                if (meta.parent / entry["file"]).is_file():
                    self.pictures[int(entry["key"])] = (None, {k: v for k, v in entry.items() if k != "key"})
            except (OSError, ValueError, KeyError, TypeError):
                pass

    def prune(self, say=print):
        """Every other version's entries, removed: none of them is ever read again."""
        for other in sorted(p for p in self.root.parent.glob("*") if p.is_dir() and p != self.root):
            shutil.rmtree(other, ignore_errors=True)
            say(f"thinkview: removed the cache of another version of the tools, {other.name}")

    def key_of(self, rec: Path) -> str:
        """The digest of a recording's bytes; OSError when its file changes while it is read."""
        st = rec.stat()
        ident = (st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns)
        with self.lock:
            if ident in self.hashes:
                return self.hashes[ident]
        key = hashlib.sha256(rec.read_bytes()).hexdigest()[:24]
        after = rec.stat()
        if (after.st_size, after.st_mtime_ns) != (st.st_size, st.st_mtime_ns):
            raise OSError("its file changed while it was read: it is being recorded")
        with self.lock:
            self.hashes[ident] = key
        return key

    def game(self, key: str):
        """THE LIST's row of a built recording, its files' names relative to the page; None when it is not built."""
        try:
            game = json.loads((self.root / key / WHOLE).read_text())
        except (OSError, ValueError):
            return None
        return dict(game, base=f"cache/{self.tool}/{key}/", pictures=f"cache/{self.tool}/{MAPS}/")

    def ensure(self, rec: Path, again=False) -> dict:
        """What the build of `rec` has come to, asking for it when it was never asked for."""
        try:
            key = self.key_of(rec)
        except OSError as e:
            return {"state": "failed", "error": f"{rec.name}: {e.strerror or e}"}
        game = self.game(key)
        if game:
            return {"state": "ready", "game": game}
        with self.lock:
            job = self.jobs.get(key)
            if job is None or (again and job["state"] == "failed"):
                job = self.jobs[key] = {"state": "queued", "what": "waiting for the builds before it", "frac": None}
                self.order.append(key)
                self.todo.put((key, rec))
            ahead = self.order.index(key) if key in self.order else 0
        return dict(job, ahead=ahead)

    def say(self, key, **state):
        with self.lock:
            self.jobs[key] = dict({"what": "", "frac": None}, **state)

    def run(self):
        while True:
            key, rec = self.todo.get()
            self.say(key, state="building", what="reading the recording")
            try:
                self.build(key, rec)
                self.say(key, state="ready")
            except (think.ThinkError, thinkview.thinklayout.LayoutError, OSError) as e:
                self.say(key, state="failed", error=str(e))
            except Exception as e:                              # noqa: BLE001
                self.say(key, state="failed", error=f"{type(e).__name__}: {e}")
            with self.lock:
                self.order.remove(key)

    def build(self, key: str, rec: Path):
        before = rec.stat()
        if self.lays is None:
            self.lays, self.builds = think.layouts([]), {}
        stem = re.sub(r"[^A-Za-z0-9._-]", "-", rec.stem) or "game"
        new = set(self.pictures)
        game, files, _lines = thinkview.build_game(
            rec, stem, self.lays, self.builds, None, dense=thinkview.DENSE_K, pictures=self.pictures,
            progress=lambda what, frac=None: self.say(key, state="building", what=what, frac=frac))
        after = rec.stat()
        if (after.st_size, after.st_mtime_ns) != (before.st_size, before.st_mtime_ns):
            raise OSError(f"{rec.name} changed while it was built: it is being recorded")
        self.say(key, state="building", what="writing the files")
        maps = self.root / MAPS
        for crc in set(self.pictures) - new:                    # a picture is one file a map, beside every entry
            raw, entry = self.pictures[crc]
            if raw:
                maps.mkdir(parents=True, exist_ok=True)
                (maps / entry["file"]).write_bytes(raw)
                (maps / (entry["file"] + ".json")).write_text(json.dumps(dict(entry, key=crc)))
                self.pictures[crc] = (None, entry)
        part = self.root / f"{key}.{os.getpid()}.part"
        shutil.rmtree(part, ignore_errors=True)
        thinkview.write_files(part, {n: b for n, b in files.items() if not n.endswith(".map.webp")})
        (part / WHOLE).write_text(json.dumps(game, separators=(",", ":")))
        shutil.rmtree(self.root / key, ignore_errors=True)
        os.replace(part, self.root / key)


class Library:
    """THE LIBRARY and the played games, as the folders hold them when asked."""

    def __init__(self, library=None, main=None):
        self.dir = thinklib.library_dir(library)
        self.main = Path(main) if main else thinklib.main_checkout()
        self.facts = {}             # {(dev, ino, size, mtime_ns): thinkview.rec_facts' answer}
        self.seen = {}              # thinklib.being_recorded's memory
        self.found = {}             # {(kind, group, name): the recording}, as the last listing found them
        self.lock = threading.Lock()

    def row(self, rec: Path, held: set) -> dict:
        st = rec.stat()
        row = {"name": rec.stem, "bytes": st.st_size, "written": st.st_mtime}
        if thinklib.being_recorded(rec, held, self.seen):
            return dict(row, state="recording")
        ident = (st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns)
        if ident not in self.facts:
            self.facts[ident] = thinkview.rec_facts(rec)
        facts = self.facts[ident]
        if facts.get("error"):
            return dict(row, state="unreadable", error=facts["error"])
        side, result = thinklib.sidecar(rec), thinkview.result_of(rec)
        who = [dict(w, rgb=list(thinkview.STOCK_RGB[w["logo"]]) if w["logo"] is not None and w["logo"] < len(thinkview.STOCK_RGB) else None)
               for w in facts["who"]]
        if result:                                              # a harness judged the game: its verdict, not the engine's
            who = [dict(w, result="won" if w["seat"] == result.get("winner") else None) for w in who]
        won = [w["seat"] for w in who if w["result"] == "won"]
        if result.get("winner") is not None:
            outcome = f"seat {result['winner']} won"
        elif result.get("outcome") == "cap":
            outcome = "ended at the cap"
        elif won:
            outcome = f"seat{'s' if len(won) > 1 else ''} {', '.join(map(str, won))} won"
        else:
            outcome = "no end recorded" if facts["ended"] is None else facts["ended"].split(":")[0].lower() \
                if facts["ended"].startswith("INCOMPLETE") else "no winner recorded"
        return dict(row, state="ready", map=facts["map"], build=facts["build"], version=facts["version"],
                    played=facts["played"], end=result.get("end_tick") or facts["end"], outcome=outcome, who=who,
                    note=side.get("note") or "", added=side.get("added"), tool=side.get("tool"),
                    game=side.get("game"))

    def listing(self) -> dict:
        with self.lock:
            held = thinklib.open_files(self.main, self.dir)
            groups, found = [], {}
            sets = [("label", row["label"], row["label"], thinklib.recordings(self.dir / row["label"]))
                    for row in thinklib.labels(self.dir)]
            sets += [("played", inst, f"played · {inst}", recs) for inst, recs in thinklib.played(self.main).items()]
            for kind, name, title, recs in sets:
                rows = []
                for rec in recs:
                    try:
                        rows.append(self.row(rec, held))
                        found[(kind, name, rec.stem)] = rec
                    except OSError:
                        continue                # gone since it was listed
                if rows:
                    rows.sort(key=lambda r: -(r.get("played") or r["written"]))
                    groups.append({"kind": kind, "name": name, "title": title, "recs": rows,
                                   "bytes": sum(r["bytes"] for r in rows),
                                   "newest": max(r.get("played") or r["written"] for r in rows)})
            groups.sort(key=lambda g: -g["newest"])
            self.found = found
            return {"groups": groups, "library": self.dir.name, "dense": thinkview.DENSE_K}

    def find(self, kind: str, group: str, name: str):
        """The recording a request names, among those a listing found; None for any other."""
        if (kind, group, name) not in self.found:
            self.listing()
        rec = self.found.get((kind, group, name))
        if rec is None:
            return None, "no such recording in the library"
        with self.lock:
            live = thinklib.being_recorded(rec, thinklib.open_files(self.main, self.dir), self.seen)
        if live:
            return None, f"{rec.name} is being recorded: it is opened when its game has ended"
        return rec, ""


class Handler(BaseHTTPRequestHandler):
    server_version = "thinkview"
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def send(self, code: int, body: bytes, ctype="application/json", cache="no-store"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", cache)
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def answer(self, doc, code=200):
        self.send(code, json.dumps(doc, separators=(",", ":")).encode())

    def do_GET(self):                                           # noqa: N802
        app = self.server.app
        host = (self.headers.get("Host") or "").rsplit(":", 1)[0]
        if host not in (HOST, "localhost"):
            return self.answer({"error": "this server answers its own address alone"}, 403)
        url = urllib.parse.urlsplit(self.path)
        q = urllib.parse.parse_qs(url.query)
        one = lambda k: (q.get(k) or [""])[0]                   # noqa: E731
        try:
            if url.path == "/":
                return self.send(200, thinkview.page(None).encode(), "text/html; charset=utf-8")
            if url.path == "/api/library":
                return self.answer(app.library.listing())
            if url.path == "/api/open":
                rec, why = app.library.find(one("k"), one("g"), one("r"))
                if rec is None:
                    return self.answer({"state": "failed", "error": why}, 404 if why.startswith("no such") else 409)
                return self.answer(app.builder.ensure(rec, again=one("again") == "1"))
            parts = url.path.split("/")
            if len(parts) == 5 and parts[:2] == ["", "cache"] and all(SAFE.fullmatch(p) for p in parts[2:]):
                root = app.builder.root.parent.resolve()
                f = (root / parts[2] / parts[3] / parts[4]).resolve()
                if root in f.parents and f.is_file() and f.suffix in TYPES and f.name != WHOLE:
                    return self.send(200, f.read_bytes(), TYPES[f.suffix], "max-age=31536000, immutable")
            return self.answer({"error": "no such thing here"}, 404)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:                                  # noqa: BLE001
            try:
                self.answer({"error": f"{type(e).__name__}: {e}"}, 500)
            except OSError:
                pass


class App:
    def __init__(self, library=None, main=None):
        self.library = Library(library, main)
        self.builder = Builder(self.library.dir / thinklib.CACHE_NAME)


def start(port=0, library=None, main=None, say=print):
    """(the server, its App), listening on 127.0.0.1 at `port` (0: one that is free), its worker started; the
    caller serves (`serve_forever`) and closes it."""
    app = App(library, main)
    app.builder.prune(say)
    app.builder.start()
    httpd = ThreadingHTTPServer((HOST, port), Handler)
    httpd.daemon_threads = True
    httpd.app = app
    return httpd, app


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="thinkview.py serve", description="the thought viewer as a local app")
    p.add_argument("--port", type=int, default=8765, help="the port on 127.0.0.1 (default 8765; 0: one that is free)")
    p.add_argument("--library", default=None, metavar="DIR", help="the library (default .data/impai/library)")
    a = p.parse_args(argv)
    try:
        httpd, app = start(a.port, a.library)
    except OSError as e:
        print(f"thinkview: COULD NOT RUN: 127.0.0.1:{a.port}: {e.strerror or e}")
        return 2
    print(f"thinkview: http://{HOST}:{httpd.server_address[1]}/  (the library {app.library.dir}, pid {os.getpid()}; "
          "Ctrl-C stops it)", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
