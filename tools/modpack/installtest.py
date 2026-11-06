#!/usr/bin/env python3
"""installtest -- the mod installer against a local HTTP server, never a real one (tagpu_modinst.c, tagpu_modtar.c,
tagpu_ufo.c; research/notes/modules/mods.md, *Download and install*).

Three parts, offline and with no game:

1. **The tar reader**, natively under the sanitizers: each way a package can lie is refused for its own reason, a truncation at
   any byte is refused, and mutated tars never crash it and never get a changed payload accepted.
2. **The archive writer**: its bytes against hpipack.build for the same tree, and the file lists it must refuse.
3. **The install**, the real module with its real WinHTTP and file handling, under Wine in a stock tacli instance. A good
   package installs, every bad download is refused with the live folder intact, a process that dies at any step leaves
   the live folder intact and a next start deletes what it left, an update swaps at that start and keeps the profile, each
   state a crash can leave in the swap resolves, a second copy of the game leaves a running install alone, and every catalog
   mod installs to the same files `modpack.py install` builds.

    installtest.py [--only NAME] [--keep]

The game's own view of an installed package is the managed-fetched-* row of tools/compat/tacompat.py.
"""
import argparse
import hashlib
import io
import json
import os
import random
import shutil
import subprocess
import sys
import tarfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DDRAW = ROOT / "tagpu/ddraw"
INSTANCES = Path(subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--path-format=absolute", "--git-common-dir"], capture_output=True, text=True).stdout.strip()).parent / "tagpu/instances"
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools"))
import hpipack  # noqa: E402
import modpack  # noqa: E402

TMP = ROOT / "tmp" / "installtest"
EXE = DDRAW / "tests/modinsttest.exe"
NATIVE = TMP / "modtartest"
INSTANCE = "modinst-test"
TACLI = ROOT / "tools/tacli"
SMALL = "prota"                 # the smallest package; every mod runs in the last case

failed = []


def report(ok, what):
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok:
        failed.append(what)


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


# ------------------------------------------------------------------ the tar reader

def block(name="a.txt", size=0, kind=b"0", prefix=b"", magic=b"ustar\0" + b"00", size_field=None, bad_sum=False, name_raw=None) -> bytes:
    h = bytearray(512)
    raw = name_raw if name_raw is not None else name.encode("latin-1")
    h[:len(raw)] = raw
    h[100:108] = b"0000644\0"
    h[108:116] = b"0000000\0"
    h[116:124] = b"0000000\0"
    h[124:136] = size_field if size_field is not None else b"%011o\0" % size
    h[136:148] = b"00000000000\0"
    h[148:156] = b" " * 8
    h[156] = kind[0] if kind else 0
    h[257:265] = magic
    h[345:345 + len(prefix)] = prefix
    h[148:156] = b"%06o\0 " % (sum(h) + (1 if bad_sum else 0))
    return bytes(h)


def member(name, data, **kw) -> bytes:
    return block(name, len(data), **kw) + data + b"\0" * (-len(data) % 512)


END = b"\0" * 1024


def run_tar(tar: bytes, declared, seed=1):
    (TMP / "t.tar").write_bytes(tar)
    (TMP / "t.tsv").write_text("".join(f"{p}\t{len(d)}\t{sha(d)}\n" for p, d in declared))
    r = subprocess.run([str(NATIVE), str(TMP / "t.tar"), str(TMP / "t.tsv"), str(seed)], capture_output=True, text=True, errors="replace")
    # the sanitizers exit 1 as well: a refusal is the exit and the word together
    if r.returncode == 0 and r.stdout.strip() == "ok":
        return "ok", ""
    if r.returncode == 1 and r.stdout.startswith("refused: "):
        return "refused", r.stdout.strip()
    return "crash", (r.stdout + r.stderr)[-400:]


def part_tar():
    src = DDRAW / "src"
    subprocess.run(["gcc", "-std=c99", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", f"-I{src}", f"-I{DDRAW / 'inc'}",
                    "-o", str(NATIVE), str(DDRAW / "tests/modtartest.c"), *[str(src / f) for f in
                    ("tagpu_modtar.c", "tagpu_modcat.c", "tagpu_modpak.c", "tagpu_json.c")]], check=True)
    a, b, e = b"hello " * 100, b"x" * 512, b""
    files = [("dir/a.txt", a), ("b.bin", b), ("empty", e)]
    good = member("dir/a.txt", a) + member("b.bin", b) + member("empty", e) + END
    got = run_tar(good, files)
    report(got[0] == "ok", f"tar: a good one is accepted ({got[1]})")
    report(run_tar(good, files, 7)[0] == "ok", "tar: in other pieces too")
    # the file python's own tarfile writes, as modpack's write_tar does
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for n, d in files:
            ti = tarfile.TarInfo(n)
            ti.size, ti.mtime, ti.mode = len(d), 0, 0o644
            t.addfile(ti, io.BytesIO(d))
    report(run_tar(buf.getvalue(), files)[0] == "ok", "tar: python's USTAR with its record padding is accepted")

    declared = files
    rest = member("b.bin", b) + member("empty", e) + END
    cases = [
        ("a parent folder", member("../evil", a) + rest, "not safe"),
        ("a parent folder inside", member("dir/../../evil", a) + rest, "not safe"),
        ("an absolute path", member("/etc/evil", a) + rest, "not safe"),
        ("a drive letter", member("C:/evil", a) + rest, "not safe"),
        ("a back slash", member("dir\\a.txt", a) + rest, "not safe"),
        ("a stream name", member("dir/a.txt:x", a) + rest, "not safe"),
        ("a trailing dot", member("dir/a.txt.", a) + rest, "not safe"),
        ("an empty component", member("dir//a.txt", a) + rest, "not safe"),
        ("a device name", member("NUL", a) + rest, "not safe"),
        ("a device name with an extension", member("dir/com1.txt", a) + rest, "not safe"),
        ("a device name by the console's", member("conout$", a) + rest, "not safe"),
        ("a name outside ASCII", member("dir/a\xe9.txt", a) + rest, "not safe"),
        ("an undeclared file", member("other.txt", a) + rest, "does not declare"),
        ("a file given twice", member("dir/a.txt", a) + member("dir/a.txt", a) + rest, "twice"),
        ("another size", member("dir/a.txt", a + b"!") + rest, "another size"),
        ("another hash", member("dir/a.txt", b"j" + a[1:]) + rest, "hash"),
        ("a symbolic link", block("dir/a.txt", 0, kind=b"2") + rest, "not a regular file"),
        ("a directory", block("dir/", 0, kind=b"5") + rest, "not a regular file"),
        ("an extended header", block("dir/a.txt", 0, kind=b"x") + rest, "not a regular file"),
        ("a long-name header", block("dir/a.txt", 0, kind=b"L") + rest, "not a regular file"),
        ("a name in the prefix", block("a.txt", len(a), prefix=b"dir") + a + b"\0" * (-len(a) % 512) + rest, "name"),
        ("a name with no end", block(name_raw=b"n" * 100) + rest, "name"),
        ("a bad checksum", block("dir/a.txt", len(a), bad_sum=True) + a + rest, "checksum"),
        ("a size in base 256", block("dir/a.txt", 0, size_field=b"\x80" + b"\0" * 11) + rest, "size"),
        ("a size past 32 bits", block("dir/a.txt", 0, size_field=b"77777777777\0") + rest, "size"),
        ("no ustar magic", block("dir/a.txt", len(a), magic=b"xxxxx\0" + b"00") + a + rest, "ustar"),
        ("padding that is not zero", block("dir/a.txt", len(a)) + a + b"Z" * (-len(a) % 512) + rest, "padding"),
        ("a declared file missing", member("dir/a.txt", a) + member("b.bin", b) + END, "lacks"),
        ("no end", good[:-1024], "no end"),
        ("half an end", good[:-512], "no end"),
        ("data after the end", good + member("b.bin", b), "after the"),
    ]
    for name, tar, want in cases:
        got = run_tar(tar, declared)
        report(got[0] == "refused" and want in got[1], f"tar refuses {name}: {got[1]}")

    # a truncation anywhere: inside a header, a member, its padding or the end
    wrong = [n for n in sorted(random.Random(1).sample(range(len(good) - 1023), 200) + list(range(0, 1100, 97))) if run_tar(good[:n], declared)[0] != "refused"]
    report(not wrong, f"tar refuses a truncation at 200+ offsets ({wrong[:5]})")

    rnd = random.Random(2)
    crashed = accepted = 0
    payload_at = 512
    for i in range(1500):
        t = bytearray(good)
        changed = False
        for _ in range(rnd.choice((1, 1, 2, 8))):
            at, v = rnd.randrange(len(t)), rnd.randrange(256)
            changed |= payload_at <= at < payload_at + len(a) and t[at] != v
            t[at] = v
        got = run_tar(bytes(t), declared, i)
        crashed += got[0] == "crash"
        accepted += changed and got[0] == "ok"
    report(not crashed and not accepted, f"tar: 1500 mutated files, {crashed} crashes, {accepted} with a changed payload accepted")


# ------------------------------------------------------------------ the server

class Server:
    """Answers one route table. A route is (mode, bytes): good, short, cut, long, stall, or absent for a 404."""
    def __init__(self):
        self.routes = {}
        self.release = threading.Event()
        outer = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                mode, data = outer.routes.get(self.path, ("404", b""))
                if mode == "404":
                    self.send_error(404)
                    return
                self.send_response(200)
                claimed = len(data) + (64 if mode == "long" else 0) if mode != "short" else len(data) - 100
                self.send_header("Content-Length", str(claimed))
                self.end_headers()
                try:
                    if mode == "good":
                        self.wfile.write(data)
                    elif mode == "short":
                        self.wfile.write(data[:-100])
                    elif mode == "long":
                        self.wfile.write(data + b"E" * 64)
                    elif mode == "cut":
                        self.wfile.write(data[:len(data) // 2])
                        self.wfile.flush()
                        self.close_connection = True
                    elif mode == "stall":
                        self.wfile.write(data[:1000])
                        self.wfile.flush()
                        outer.release.wait(120)
                        self.wfile.write(data[1000:])
                except OSError:
                    pass

        self.http = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.http.daemon_threads = True
        threading.Thread(target=self.http.serve_forever, daemon=True).start()
        self.base = f"http://127.0.0.1:{self.http.server_address[1]}"


# ------------------------------------------------------------------ the install

class Wine:
    def __init__(self):
        subprocess.run([sys.executable, str(TACLI), "rm", INSTANCE, "--force"], capture_output=True)
        r = subprocess.run([sys.executable, str(TACLI), "create", INSTANCE, "--display", ":99", "--res", "1024x768", "--private-registry"], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"installtest: tacli create: {r.stderr or r.stdout}")
        meta = json.loads((INSTANCES / INSTANCE / "instance.json").read_text())
        self.game, self.prefix = Path(meta["gamedir"]), Path(meta["prefix"])
        self.env = {k: v for k, v in os.environ.items() if k != "DISPLAY"}
        self.env.update(WINEPREFIX=str(self.prefix), WINEDEBUG="-all")
        subprocess.run(["wineserver", "-p60"], env=self.env, capture_output=True)
        self.mods = self.game / "mods"

    def popen(self, *args, stop=None):
        env = dict(self.env)
        if stop:
            env["MODINST_STOP"] = stop
        return subprocess.Popen(["wine", str(EXE), *map(str, args)], cwd=self.game, env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

    def run(self, *args, stop=None):
        p = self.popen(*args, stop=stop)
        out, _ = p.communicate(timeout=300)
        return p.returncode, out

    def clean(self):
        shutil.rmtree(self.mods, ignore_errors=True)
        self.mods.mkdir()

    def close(self):
        subprocess.run(["wineserver", "-k"], env=self.env, capture_output=True)
        subprocess.run([sys.executable, str(TACLI), "rm", INSTANCE, "--force"], capture_output=True)


def tree_hashes(folder: Path) -> dict:
    return {str(p.relative_to(folder)): sha(p.read_bytes()) for p in sorted(folder.rglob("*")) if p.is_file()}


def plant_live(w, mid, version="0.9"):
    d = w.mods / mid
    (d / "sub").mkdir(parents=True)
    (d / "old.hpi").write_bytes(b"the old version")
    (d / "sub/old.txt").write_text("old")
    (d / "installed.txt").write_text(version)
    return tree_hashes(d)


def part_install(w, srv, only):
    catalog = json.loads((ROOT / "mods/catalog.json").read_text())
    # the reference: what modpack.py install makes of each package, as it stands
    for m in catalog["mods"]:
        scratch = TMP / "reference"
        shutil.rmtree(scratch, ignore_errors=True)
        scratch.mkdir(parents=True)
        subprocess.run([sys.executable, str(HERE / "modpack.py"), "install", m["id"], str(scratch), "--link"], check=True, capture_output=True)
        shutil.rmtree(scratch)
    entry = next(m for m in catalog["mods"] if m["id"] == SMALL)
    pkg = entry["packages"][0]
    real = (modpack.OUT / SMALL / f"{SMALL}.tar.zst").read_bytes()
    url = "/" + pkg["url"]
    catpath = w.game / "cat.json"
    catpath.write_text(json.dumps(catalog))
    installed_ref = modpack.OUT / SMALL / "installed"

    def serve(mode="good", data=real):
        srv.routes.clear()
        srv.routes[url] = (mode, data)

    def install(stop=None, base=None):
        return w.run("install", "cat.json", SMALL, srv.base if base is None else base, stop=stop)

    def case(name):
        return not only or only in name

    if case("good"):
        w.clean()
        serve()
        rc, out = install()
        new = w.mods / f"{SMALL}.new"
        report(rc == 0 and "ready" in out, f"good: it installs ({out.strip().splitlines()[-1] if out.strip() else ''})")
        report(new.is_dir() and not (w.mods / SMALL).exists(), "good: it waits in .new, with no live folder made")
        report(not (w.mods / f"{SMALL}.part").exists(), "good: the .part is gone")
        report((new / "installed.txt").read_text() == entry["version"], "good: the marker names the catalog's version")
        got, want = tree_hashes(new), tree_hashes(installed_ref)
        got.pop("installed.txt", None)
        got_hpi, want_hpi = got.pop(f"{SMALL}.originals.hpi", None), want.pop(f"{SMALL}.originals.hpi", None)
        report(got == want and got_hpi and want_hpi, f"good: the files are the ones modpack.py install makes ({len(got)})")
        rc, _ = w.run("sweep", "cat.json")
        report(rc == 0 and (w.mods / SMALL / f"{SMALL}.hpi").is_file() and not new.exists(), "good: a start swaps it in")
        rc, out = w.run("installed", SMALL)
        report(out.strip() == entry["version"], f"good: the installed version reads {out.strip()}")

    if case("refused"):
        corrupt = bytearray(real)
        corrupt[len(corrupt) // 2] ^= 0x40
        for mode, data, want in (("good", bytes(corrupt), "damaged"), ("short", real, "stopped"), ("cut", real, "stopped"), ("long", real, "more than"),
                                 ("404", b"", "answered 404")):
            w.clean()
            live = plant_live(w, SMALL)
            serve(mode, data)
            rc, out = install()
            report(rc == 1 and want in out, f"refused {mode if data == real or mode == '404' else 'corrupt'}: {out.strip().splitlines()[-1]}")
            report(tree_hashes(w.mods / SMALL) == live and not list(w.mods.glob("*.part")) and not list(w.mods.glob("*.new")),
                   f"refused {mode}: the live folder is intact, nothing is left")
        w.clean()
        catalog2 = json.loads(json.dumps(catalog))
        next(m for m in catalog2["mods"] if m["id"] == SMALL)["packages"][0]["url"] = "relative/only.tar.zst"
        catpath.write_text(json.dumps(catalog2))
        rc, out = w.run("install", "cat.json", SMALL, "")
        report(rc == 1 and "does not yet name" in out, f"refused no address: {out.strip().splitlines()[-1]}")
        for label, change, want in (("a file path out of the folder", lambda p: p["files"][0].update(path="../evil"), "safe path"),
                                    ("a hash that is not one", lambda p: p.update(sha256="abc"), "url, hash"),
                                    ("a size past 2 GB", lambda p: p.update(size=3e9), "url, hash"),
                                    ("no files", lambda p: p.update(files=[]), "url, hash"),
                                    ("a file posing as the marker", lambda p: p["files"][0].update(path="Installed.txt"), "declares a file named")):
            cat = json.loads(json.dumps(catalog))
            change(next(m for m in cat["mods"] if m["id"] == SMALL)["packages"][0])
            catpath.write_text(json.dumps(cat))
            rc, out = w.run("install", "cat.json", SMALL, srv.base)
            report(rc == 1 and want in out and not list(w.mods.glob("*.new")), f"refused catalog with {label}: {out.strip()}")
        catpath.write_text(json.dumps(catalog))
        rc, out = w.run("install", "cat.json", "nosuch", srv.base)
        report(rc == 1 and "no entry" in out, f"refused unknown mod: {out.strip()}")
        catpath.write_text(json.dumps(catalog))

    if case("package"):
        # the outer hash passes, as a catalog of a damaged mirror could: the reader inside still has to refuse
        def rebuilt(mutate, label, want):
            with tarfile.open(fileobj=io.BytesIO(subprocess.run(["zstd", "-dqc"], input=real, capture_output=True, check=True).stdout)) as t:
                members = {m.name: t.extractfile(m).read() for m in t.getmembers()}
            tar = mutate(members)
            z = subprocess.run(["zstd", "-qc", "-3"], input=tar, capture_output=True, check=True).stdout
            cat = json.loads(json.dumps(catalog))
            p = next(m for m in cat["mods"] if m["id"] == SMALL)["packages"][0]
            p["sha256"], p["size"] = sha(z), len(z)
            catpath.write_text(json.dumps(cat))
            w.clean()
            live = plant_live(w, SMALL)
            serve("good", z)
            rc, out = install()
            report(rc == 1 and want in out, f"package {label}: {out.strip().splitlines()[-1]}")
            report(tree_hashes(w.mods / SMALL) == live and not list(w.mods.glob("*.new")), f"package {label}: the live folder is intact")
        def as_tar(members, extra=b""):
            return b"".join(member(n, d) for n, d in members.items()) + extra + END
        rebuilt(lambda m: as_tar({"../evil.txt": b"x", **m}), "with a parent folder", "not safe")
        rebuilt(lambda m: as_tar({"/evil.txt": b"x", **m}), "with an absolute path", "not safe")
        rebuilt(lambda m: as_tar({"undeclared.txt": b"x", **m}), "with an undeclared file", "does not declare")
        rebuilt(lambda m: as_tar(m)[:-2000], "truncated", "archive")
        rebuilt(lambda m: as_tar({**m, f"{SMALL}.originals.tsv": m[f"{SMALL}.originals.tsv"][:-1] + b"0"}), "with a changed file", "hash")
        catpath.write_text(json.dumps(catalog))

    if case("killed"):
        for stop, label in ((f"download:{len(real) // 2}", "during the download"), ("unpack:2", "during the unpack"), ("recipe:5", "during the recipe"),
                            ("marker:0", "before the marker"), ("retired:0", "with a waiting update's marker just deleted")):
            w.clean()
            live = plant_live(w, SMALL)
            if stop.startswith("retired"):
                (w.mods / f"{SMALL}.new").mkdir()
                (w.mods / f"{SMALL}.new/waiting.txt").write_text("a finished update")
                (w.mods / f"{SMALL}.new/installed.txt").write_text("0.95")
            serve()
            rc, out = install(stop=stop)
            left = sorted(p.name for p in w.mods.iterdir())
            report(rc != 0 and "ready" not in out and tree_hashes(w.mods / SMALL) == live, f"killed {label}: the live folder is intact, {left} left")
            report(not (w.mods / f"{SMALL}.new/installed.txt").exists(), f"killed {label}: no marker")
            rc, _ = w.run("sweep", "cat.json")
            report(sorted(p.name for p in w.mods.iterdir()) == [SMALL] and tree_hashes(w.mods / SMALL) == live, f"killed {label}: the next start deletes what it left")

    if case("update"):
        w.clean()
        live = plant_live(w, SMALL, "1.0")
        prof = w.game / "profiles" / f"{SMALL}.ini"
        prof.parent.mkdir(exist_ok=True)
        prof.write_text("[play]\nthe player's choices\n")
        before = prof.read_bytes()
        serve()
        rc, out = install()
        report(rc == 0 and tree_hashes(w.mods / SMALL) == live, "update: it waits, and the live folder is the old version until the start")
        rc, out = w.run("installed", SMALL)
        report(out.strip() == "1.0", "update: until then the old version is what is installed")
        w.run("sweep", "cat.json")
        rc, out = w.run("installed", SMALL)
        report(out.strip() == entry["version"] and not (w.mods / SMALL / "sub").exists() and not (w.mods / SMALL / "old.hpi").exists(), f"update: at the start it is the new one, {out.strip()}, none of the old left")
        report(prof.read_bytes() == before, "update: the profile is untouched")
        serve()
        (w.mods / f"{SMALL}.new").mkdir()
        (w.mods / f"{SMALL}.new/installed.txt").write_text("an earlier wait")
        (w.mods / f"{SMALL}.new/stale.txt").write_text("stale")
        rc, out = install()
        report(rc == 0 and not (w.mods / f"{SMALL}.new/stale.txt").exists(), "update: a second install replaces a waiting one")
        # the waiting update's marker cannot be deleted: nothing of it may go, or the sweep would take what is left for a whole version
        waiting = tree_hashes(w.mods / f"{SMALL}.new")
        holder = w.popen("hold", f"mods\\{SMALL}.new\\installed.txt")
        held = holder.stdout.readline().strip()
        rc, out = install()
        holder.kill()
        holder.wait()
        report(held == "held" and rc == 1 and "stays as it is" in out and tree_hashes(w.mods / f"{SMALL}.new") == waiting,
               f"update: with its marker held open, a waiting update is kept whole: {out.strip().splitlines()[-1]}")
        prof.unlink()

    if case("swap"):
        def state(live=None, old=None, fresh=None, marker=True):
            w.clean()
            for name, tag in ((SMALL, live), (f"{SMALL}.old", old), (f"{SMALL}.new", fresh)):
                if tag:
                    (w.mods / name).mkdir()
                    (w.mods / name / "tag").write_text(tag)
            if fresh and marker:
                (w.mods / f"{SMALL}.new/installed.txt").write_text("2")
            w.run("sweep", "cat.json")
            names = sorted(p.name for p in w.mods.iterdir())
            tag = (w.mods / SMALL / "tag").read_text() if (w.mods / SMALL / "tag").exists() else None
            return names, tag
        for label, kw, want in (("live and a finished new", dict(live="L", fresh="N"), "N"),
                                ("live, an old and a finished new", dict(live="L", old="O", fresh="N"), "N"),
                                ("the live moved aside, the new not yet in place", dict(old="O", fresh="N"), "N"),
                                ("the new in place, the old not yet deleted", dict(live="N", old="O"), "N"),
                                ("the live moved aside, no new", dict(old="O"), "O"),
                                ("the live moved aside, an unfinished new", dict(old="O", fresh="N", marker=False), "O"),
                                ("live and an unfinished new", dict(live="L", fresh="N", marker=False), "L")):
            names, tag = state(**kw)
            report(names == [SMALL] and tag == want, f"swap: {label}: the live folder holds {tag}, {names}")
        # names this module did not make are the player's
        w.clean()
        for d in ("Foo.new", "Foo.old", "bar"):
            (w.mods / d).mkdir()
        (w.mods / "bar.part").write_text("x")
        (w.mods / f"{SMALL}.part").write_text("x")
        w.run("sweep", "cat.json")
        names = sorted(p.name for p in w.mods.iterdir())
        report(names == sorted(["Foo.new", "Foo.old", "bar", "bar.part"]), f"swap: names outside the catalog are left alone, and its own .part goes: {names}")

    if case("lock"):
        w.clean()
        serve("stall")
        srv.release.clear()
        p = w.popen("install", "cat.json", SMALL, srv.base)
        deadline = time.time() + 60
        while time.time() < deadline and not (w.mods / f"{SMALL}.part").exists():
            time.sleep(0.05)
        report((w.mods / f"{SMALL}.part").exists(), "lock: a second copy starts while one is downloading")
        rc, out = w.run("sweep", "cat.json")
        report("another copy" in out and (w.mods / f"{SMALL}.new").is_dir() and (w.mods / f"{SMALL}.part").exists(), f"lock: its sweep leaves the install alone: {out.strip()[:80]}")
        rc, out = install()
        report(rc == 1 and "another copy" in out, f"lock: its own install is refused: {out.strip()}")
        srv.release.set()
        out, _ = p.communicate(timeout=120)
        report(p.returncode == 0 and "ready" in out, "lock: the first install still finishes")
        rc, out = w.run("sweep", "cat.json")
        report((w.mods / SMALL).is_dir() and not list(w.mods.glob("install.lock")), "lock: released, and a start swaps it")
        srv.release.clear()

    if case("archives"):
        serve()
        arc = "rev31.gp3"
        path = next(p for p in w.game.iterdir() if p.name.lower() == arc)
        keep = w.game / "kept-archive"
        path.rename(keep)
        try:
            w.clean()
            rc, out = install()
            report(rc == 1 and f"no {arc}" in out and not list(w.mods.glob("*.new")), f"archives: one missing is named: {out.strip().splitlines()[-1]}")
            path.write_bytes(hpipack.build({"other": {"x.txt": b"x"}}, 0, 1))
            w.clean()
            rc, out = install()
            report(rc == 1 and "not the game's" in out and not list(w.mods.glob("*.new")), f"archives: another with its name is refused: {out.strip().splitlines()[-1]}")
            path.unlink()
        finally:
            keep.rename(path)

    if case("attach"):
        # the real DLL's attach: only the game acts, and only on its own folder, whatever the current directory
        w.clean()
        (w.mods / f"{SMALL}.part").write_text("the current directory's")
        other = w.game / "attachtest"
        shutil.rmtree(other, ignore_errors=True)
        (other / "mods" / f"{SMALL}.new").mkdir(parents=True)
        (other / "mods" / f"{SMALL}.part").write_text("x")
        shutil.copy2(DDRAW / "ddraw.dll", other / "ddraw.dll")
        for name in ("tool.exe", "TotalA.exe"):
            shutil.copy2(EXE, other / name)
        def at(name):
            # Wine's own ddraw comes first unless told otherwise, and then none of this has run: the log below says it did
            shutil.rmtree(other / "log", ignore_errors=True)
            p = subprocess.Popen(["wine", str(other / name), "load-dll"], cwd=w.game, env={**w.env, "WINEDLLOVERRIDES": "ddraw=n,b"}, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
            try:
                return p.communicate(timeout=120)[0].strip()
            except subprocess.TimeoutExpired:
                p.kill()
                return "timed out"
        said = at("tool.exe")
        report((other / "log").is_dir() and (other / "mods" / f"{SMALL}.new").is_dir() and (other / "mods" / f"{SMALL}.part").exists(), f"attach: an exe that is not the game leaves its folder alone ({said})")
        said = at("TotalA.exe")
        report((other / "log").is_dir() and not (other / "mods" / f"{SMALL}.new").exists() and not (other / "mods" / f"{SMALL}.part").exists(), f"attach: the game settles its own folder ({said})")
        report((w.mods / f"{SMALL}.part").exists(), "attach: and never the current directory's")
        shutil.rmtree(other)

    if case("every"):
        for m in catalog["mods"]:
            mid = m["id"]
            w.clean()
            data = (modpack.OUT / mid / f"{mid}.tar.zst").read_bytes()
            srv.routes.clear()
            srv.routes["/" + m["packages"][0]["url"]] = ("good", data)
            t0 = time.time()
            rc, out = w.run("install", "cat.json", mid, srv.base)
            took = time.time() - t0
            new = w.mods / f"{mid}.new"
            ok = rc == 0 and (new / "installed.txt").is_file()
            got, want = tree_hashes(new), tree_hashes(modpack.OUT / mid / "installed")
            got.pop("installed.txt", None)
            hg, hw = new / f"{mid}.originals.hpi", modpack.OUT / mid / "installed" / f"{mid}.originals.hpi"
            got.pop(hg.name, None)
            want.pop(hg.name, None)
            ga, wa = hpipack.Archive(hg), hpipack.Archive(hw)
            same = {k: sha(ga.read(k)) for k in ga.files} == {k: sha(wa.read(k)) for k in wa.files}
            report(ok and got == want and same and len(ga.files) > 0,
                   f"every: {mid} {len(data) / 1e6:.1f} MB in {took:.1f} s: {len(got)} files and {len(ga.files)} originals as modpack.py install makes them ({out.strip().splitlines()[-1]})")


def part_ufo(w):
    d = w.game / "ufotest"
    shutil.rmtree(d, ignore_errors=True)
    names = {"a/b/c.txt": b"c" * 70000, "A/b/d.txt": b"d", "a/e.txt": b"", "top.txt": b"t" * 10, "a.b/x": b"x", "z_/y": b"y", "Z/deep/1/2/3/4/f": b"f"}
    for n, data in names.items():
        (d / n).parent.mkdir(parents=True, exist_ok=True)
        (d / n).write_bytes(data)
    rel = [f"ufotest/{n}" for n in names]
    # the disk folds nothing here, so the test's tree is built the way insert_cased builds it, without case
    tree = {}
    for n in sorted(rel, key=lambda s: s.lower().replace("/", "\x01")):
        modpack.insert_cased(tree, n, (w.game / n).read_bytes())
    rc, _ = w.run("ufo", "out.ufo", *rel)
    got = (w.game / "out.ufo").read_bytes()
    want = hpipack.build(tree, 0, 1)
    report(rc == 0 and got == want, f"writer: the bytes hpipack builds for a tree of any depth ({len(got)} B)")
    a = hpipack.Archive(w.game / "out.ufo")
    report(len(a.files) == len(rel), "writer: the python reader finds every file")
    (w.game / "same").write_bytes(b"1")
    (w.game / "SAME").mkdir(exist_ok=True)
    (w.game / "SAME/f").write_bytes(b"2")
    rc, _ = w.run("ufo", "bad.ufo", "same", "same")
    report(rc == 1, "writer: refuses a file given twice")
    rc, _ = w.run("ufo", "bad.ufo", "same", "SAME/f")
    report(rc == 1, "writer: refuses a path that is both a file and a folder")
    deep = "/".join(["d"] * 20) + "/f"
    (w.game / deep).parent.mkdir(parents=True)
    (w.game / deep).write_bytes(b"f")
    rc, _ = w.run("ufo", "bad.ufo", deep)
    report(rc == 1, "writer: refuses a tree past its depth")
    shutil.rmtree(w.game / "d")
    shutil.rmtree(d)
    shutil.rmtree(w.game / "SAME")
    (w.game / "same").unlink()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--only", default="", help="the cases whose name holds this: tar, writer, good, refused, package, killed, update, swap, lock, archives, attach, every")
    ap.add_argument("--keep", action="store_true", help="keep the instance")
    a = ap.parse_args()
    TMP.mkdir(parents=True, exist_ok=True)
    subprocess.run(["make", "-C", str(DDRAW), f"-j{os.cpu_count()}", "tests/modinsttest.exe"], check=True, capture_output=True)
    if not a.only or a.only == "tar":
        part_tar()
    if a.only != "tar":
        w, srv = Wine(), Server()
        try:
            if not a.only or a.only == "writer":
                part_ufo(w)
            if a.only != "writer":
                part_install(w, srv, a.only)
        finally:
            srv.release.set()
            if not a.keep:
                w.close()
    print(f"installtest: {'all passed' if not failed else f'{len(failed)} FAILED'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
