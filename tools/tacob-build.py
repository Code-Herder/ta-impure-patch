#!/usr/bin/env python3
"""tacob-build.py — build the tacob folder that needs no Python (landing 5).

    tools/tacob-build.py vendor           # fetch the page's JavaScript, pinned and checksummed
    tools/tacob-build.py wine-setup       # a Wine prefix with a Windows Python and PyInstaller
    tools/tacob-build.py build            # PyInstaller onedir -> dist/tacob/
    tools/tacob-build.py check            # run the built folder in a prefix that has no Python

This runs on the *host* (Linux, stdlib only). The Windows build happens inside a
Wine prefix, because there is no remote and no CI (`tacob-design.md` §"Decisions
locked", the "The exe" row): a Windows Python is installed into a prefix of our
own and PyInstaller runs there.

Two rules the vendor step exists for:

  * **Nothing is vendored into git.** `tools/vendor/` is gitignored; what *is*
    tracked is `tools/tacob-vendor.json`, the manifest — every URL with the
    sha256 of the bytes we fetched. A later fetch that returns different bytes
    fails instead of shipping.
  * **The CodeMirror files keep their bare imports.** The page's import map
    resolves them, and jsdelivr's `+esm` bundles would inline a second copy of
    `@codemirror/state`, which breaks CodeMirror. So the fetch mirrors the CDN's
    own paths and follows each file's *relative* imports; the server then swaps
    one prefix (`tacob.rewrite_import_map`).
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.parse
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
VENDOR = HERE / "vendor"
MANIFEST = HERE / "tacob-vendor.json"
EDITOR = HERE / "tacob-edit.html"
CDN = "https://cdn.jsdelivr.net/"

# Where the Windows toolchain lives. Outside the repo — it is a gigabyte of
# somebody else's software — and overridable, because it is a machine's state and
# not the project's.
PREFIX = Path(os.environ.get("TACOB_WINEPREFIX")
              or Path.home() / ".local/share/tacob-build/prefix")
WINE_PY = r"C:\Python311\python.exe"
PYTHON_URL = "https://www.python.org/ftp/python/3.11.9/python-3.11.9-amd64.exe"

RELATIVE_IMPORT = re.compile(r"""(?:^|[\s;])(?:import|export)[^'"()]*?['"](\.{1,2}/[^'"]+)['"]"""
                             r"""|import\(\s*['"](\.{1,2}/[^'"]+)['"]\s*\)""", re.M)


# --------------------------------------------------------------------------- vendor

def import_map(text: str) -> dict:
    match = re.search(r'<script\s+type="importmap"\s*>(.*?)</script>', text, re.S)
    if not match:
        raise SystemExit("tacob-build: no import map in tacob-edit.html")
    return json.loads(match.group(1))["imports"]


def seeds() -> list:
    """Every URL the page needs, from its own import map.

    A trailing slash in a map is a *prefix*, not a file — `three/addons/` cannot
    be enumerated from the map — so for those the seeds are the addon modules the
    page actually imports, scraped from its own `import` lines. Their own
    relative imports are followed from there."""
    text = EDITOR.read_text(encoding="utf-8")
    urls, prefixes = [], {}
    for specifier, url in import_map(text).items():
        if not url.startswith(CDN):
            raise SystemExit(f"tacob-build: {specifier} is not on {CDN} — "
                             "the vendor step only knows that one CDN")
        (prefixes.__setitem__(specifier, url) if url.endswith("/") else urls.append(url))
    for specifier, base in prefixes.items():
        pattern = re.compile(r"""['"]""" + re.escape(specifier) + r"""([\w./-]+)['"]""")
        found = sorted(set(pattern.findall(text)))
        if not found:
            print(f"  note: nothing imports {specifier} — nothing fetched for it")
        urls += [base + tail for tail in found]
    return urls


def fetch(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "tacob-build"})
    with urllib.request.urlopen(request, timeout=60) as response:
        if response.status != 200:
            raise SystemExit(f"tacob-build: {url} -> HTTP {response.status}")
        return response.read()


def relative_imports(source: str):
    for a, b in RELATIVE_IMPORT.findall(source):
        yield a or b


def cmd_vendor(args):
    """Fetch every module the page imports into `tools/vendor/`, mirroring the
    CDN's own paths, and verify each against the manifest."""
    manifest = json.loads(MANIFEST.read_text()) if MANIFEST.is_file() else {"files": {}}
    known = manifest.get("files", {})
    fresh, queue, done = {}, list(seeds()), set()
    while queue:
        url = queue.pop(0)
        if url in done:
            continue
        done.add(url)
        rel = url[len(CDN):]
        blob = fetch(url)
        digest = hashlib.sha256(blob).hexdigest()
        pinned = known.get(rel)
        if pinned and pinned["sha256"] != digest and not args.update:
            raise SystemExit(
                f"tacob-build: {rel} changed under the pin\n"
                f"  manifest {pinned['sha256']}\n  fetched  {digest}\n"
                "  the point of the manifest is that this stops the build; "
                "pass --update once you have looked at why")
        target = VENDOR / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(blob)
        fresh[rel] = {"url": url, "sha256": digest, "bytes": len(blob)}
        base = url.rsplit("/", 1)[0] + "/"
        for spec in relative_imports(blob.decode("utf-8", "replace")):
            queue.append(_join(base, spec))
    manifest = {"note": "pinned copies of the editor page's JavaScript; the files "
                        "themselves live in tools/vendor/, which is gitignored "
                        "(tools/tacob-build.py vendor)",
                "origin": CDN, "files": dict(sorted(fresh.items()))}
    MANIFEST.write_text(json.dumps(manifest, indent=1) + "\n")
    total = sum(f["bytes"] for f in fresh.values())
    print(f"{len(fresh)} files, {total} bytes -> {VENDOR}")
    print(f"manifest {MANIFEST}")
    missing = sorted(set(known) - set(fresh))
    if missing:
        print(f"  dropped from the manifest: {', '.join(missing)}")


def _join(base: str, spec: str) -> str:
    """`urljoin`, wrapped so a `../` that walks out of the CDN's tree is an error
    and not a surprise."""
    url = urllib.parse.urljoin(base, spec)
    if not url.startswith(CDN):
        raise SystemExit(f"tacob-build: {spec} from {base} leaves {CDN}")
    return url


def cmd_verify(args):
    """The manifest against what is on disk — the check the build runs first."""
    if not MANIFEST.is_file():
        raise SystemExit("tacob-build: no manifest; run `vendor` first")
    files = json.loads(MANIFEST.read_text())["files"]
    bad = []
    for rel, entry in files.items():
        path = VENDOR / rel
        if not path.is_file():
            bad.append(f"{rel}: missing")
        elif hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            bad.append(f"{rel}: sha256 differs")
    for line in bad:
        print(f"  {line}")
    print(f"{len(files) - len(bad)} of {len(files)} vendored files match the manifest")
    if bad:
        raise SystemExit(1)


# --------------------------------------------------------------------------- wine

def wine(*command, prefix=None, check=True, quiet=False, cwd=None):
    """One Wine call, on a display of its own. Returns `(exit code, output)`.

    Two things here are not decoration. The display is `xvfb-run`'s, never the
    owner's — the Python installer and PyInstaller both put windows up. And
    stdin is `/dev/null` while stdout is always a **pipe**, even when we are only
    going to print it again: a Windows Python whose stdout is a plain file dies
    in Wine before it runs a line —

        Fatal Python error: init_sys_streams: can't initialize sys standard
        streams / OSError: [WinError 6] Invalid handle

    — which is exactly what happens the first time somebody logs a build to a
    file. Measured 2026-09-07, on `pip install` and again on this build."""
    env = dict(os.environ, WINEPREFIX=str(prefix or PREFIX), WINEARCH="win64",
               WINEDEBUG="-all", WINEDLLOVERRIDES="mscoree,mshtml=d")
    argv = (["xvfb-run", "-a"] if shutil.which("xvfb-run") else []) + ["wine", *command]
    proc = subprocess.Popen(argv, env=env, cwd=cwd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors="replace", bufsize=1)
    lines = []
    for line in proc.stdout:
        lines.append(line)
        if not quiet:
            sys.stdout.write(line)
            sys.stdout.flush()
    code = proc.wait()
    if check and code:
        raise SystemExit(f"tacob-build: {command[0]} exited {code}")
    return code, "".join(lines)


def cmd_wine_setup(args):
    """A prefix with a Windows Python, PyInstaller and pywebview in it."""
    downloads = PREFIX.parent / "dl"
    downloads.mkdir(parents=True, exist_ok=True)
    installer = downloads / PYTHON_URL.rsplit("/", 1)[-1]
    if not installer.is_file():
        print(f"fetching {PYTHON_URL}")
        installer.write_bytes(fetch(PYTHON_URL))
    if not (PREFIX / "drive_c/windows").is_dir():
        print(f"creating {PREFIX}")
        PREFIX.mkdir(parents=True, exist_ok=True)
        wine("wineboot", "-u")
    if not (PREFIX / "drive_c/Python311/python.exe").is_file():
        print("installing Python 3.11 into the prefix")
        wine(str(installer), "/quiet", "InstallAllUsers=0", "PrependPath=0",
             "Include_test=0", "Include_launcher=0", "SimpleInstall=1",
             r"TargetDir=C:\Python311")
    wine(WINE_PY, "-m", "pip", "install", "--disable-pip-version-check", "--no-input",
         "pyinstaller", "pywebview")
    _code, out = wine(WINE_PY, "-m", "pip", "list", quiet=True)
    print(out)


# --------------------------------------------------------------------------- build

def cmd_build(args):
    """PyInstaller onedir, from `tools/tacob.spec`, into `dist/tacob/`."""
    if not (PREFIX / "drive_c/Python311/python.exe").is_file():
        raise SystemExit(f"tacob-build: no Windows Python in {PREFIX} — run `wine-setup`")
    if not args.no_vendor:
        cmd_verify(args)
    dist = ROOT / "dist"
    if args.clean and (dist / "tacob").is_dir():
        shutil.rmtree(dist / "tacob")
    # Relative paths, run from the checkout root: Wine maps the working directory,
    # and nothing absolute from this machine ends up in the build log or the spec.
    wine(WINE_PY, "-m", "PyInstaller", "--noconfirm", "--distpath", "dist",
         "--workpath", "build/pyinstaller", "tools/tacob.spec", cwd=ROOT)
    folder = dist / "tacob"
    exe = folder / "tacob.exe"
    if not exe.is_file():
        raise SystemExit(f"tacob-build: PyInstaller wrote no {exe}")
    size = sum(p.stat().st_size for p in folder.rglob("*") if p.is_file())
    count = sum(1 for p in folder.rglob("*") if p.is_file())
    print(f"{folder}: {count} files, {size / 1e6:.1f} MB")


def load_ta3do():
    """The asset layer, for the two things the check needs from it: where the
    checkout's game folder is (it knows a worktree's data lives in the main
    checkout) and a private X display for headless Chrome."""
    import importlib.util
    from importlib.machinery import SourceFileLoader
    loader = SourceFileLoader("ta3do", str(HERE / "ta3do"))
    spec = importlib.util.spec_from_loader("ta3do", loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def clean_prefix(args):
    """A Wine prefix with no Python in it — the stand-in for the gate's "machine
    with no Python", and checked rather than assumed."""
    prefix = Path(args.prefix) if args.prefix else PREFIX.parent / "clean-prefix"
    if not (prefix / "drive_c/windows").is_dir():
        print(f"creating a Python-free prefix at {prefix}")
        prefix.mkdir(parents=True, exist_ok=True)
        wine("wineboot", "-u", prefix=prefix)
    strays = sorted((prefix / "drive_c").rglob("python*.exe"))
    if strays:
        raise SystemExit(f"tacob-build: {prefix} has a Python in it ({strays[0]}) — "
                         "the gate is a machine that does not")
    print(f"prefix {prefix}: no python.exe anywhere under drive_c")
    return prefix


def drive_page(url, offline=True, png=None):
    """Load the served page in headless Chrome and report what came back.

    The same helpers landing 4 used, for the same reason: `ta3do.Display()` is a
    private Xvfb, never the owner's desktop. `--host-resolver-rules` cuts the page
    off from every host but loopback, which is what turns "it loaded" into "it
    loaded *from the folder*"."""
    import tempfile
    ta3do = load_ta3do()
    display = ta3do.Display()
    try:
        with tempfile.TemporaryDirectory(prefix="tacob-check-") as profile:
            cmd = [ta3do.find_chrome(), "--headless=new", "--no-sandbox",
                   "--disable-dev-shm-usage", f"--user-data-dir={profile}",
                   "--use-gl=angle", "--use-angle=swiftshader",
                   "--enable-unsafe-swiftshader", "--hide-scrollbars",
                   "--window-size=1400,900", "--virtual-time-budget=9000"]
            if offline:
                cmd.append("--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1")
            if png:
                cmd.append(f"--screenshot={png}")
            env = dict(os.environ, **({"DISPLAY": display.name} if display.name else {}))
            proc = subprocess.run(cmd + ["--dump-dom", url], env=env, timeout=180,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    finally:
        display.close()
    dom = proc.stdout.decode("utf-8", "replace")
    if "<html" not in dom.lower():
        raise SystemExit("tacob-build: chrome dumped no DOM: "
                         + proc.stderr.decode("latin-1", "replace")[-400:])
    return dom


def cmd_check(args):
    """Run the built folder in a prefix that has no Python — the landing's gate.

    Two runs. The headless smoke test proves the bundle *is* the program: it
    reads the game's archives, decompiles, compiles, and steps the VM. The page
    run proves the half that only a browser can prove — that `tacob-edit.html`,
    its vendored JavaScript and the pose stream all survived the packaging — and
    it runs with the network cut off, so a CDN cannot answer for the folder."""
    folder = ROOT / "dist/tacob"
    exe = folder / "tacob.exe"
    if not exe.is_file():
        raise SystemExit(f"tacob-build: nothing built at {exe} — run `build`")
    prefix = clean_prefix(args)
    gamedir = str(args.gamedir or load_ta3do().game_dir())
    if args.args:
        raise SystemExit(wine(str(exe), *args.args, prefix=prefix, check=False)[0])

    print(f"\n--- {exe.name} serve {args.unit} --ticks 30 ---")
    wine(str(exe), "serve", args.unit, "--ticks", "30", "--gamedir", gamedir,
         prefix=prefix)

    print(f"\n--- {exe.name} gui {args.unit} --no-open, driven with the network cut off ---")
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEARCH="win64", WINEDEBUG="-all",
               WINEDLLOVERRIDES="mscoree,mshtml=d")
    argv = (["xvfb-run", "-a"] if shutil.which("xvfb-run") else []) + \
        ["wine", str(exe), "gui", args.unit, "--no-open", "--gamedir", gamedir]
    proc = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors="replace", bufsize=1)
    url, said = None, []
    try:
        for line in proc.stdout:
            said.append(line)
            print("  " + line.rstrip())
            found = re.search(r"http://127\.0\.0\.1:\d+", line)
            if found:
                url = found.group(0)
            if "ctrl-c to stop" in line:
                break
        if not url:
            raise SystemExit("tacob-build: the built folder printed no URL\n"
                             + "".join(said[-10:]))
        png = Path(args.shot) if args.shot else None
        dom = drive_page(url, offline=not args.online, png=png)
    finally:
        proc.terminate()
    counts = {"CodeMirror": dom.count('class="cm-editor'),
              "textarea fallback": dom.count("<textarea"),
              "thread cells": dom.count('class="t '),
              "canvas": dom.count("<canvas")}
    status = re.search(r'id="status"[^>]*>(.*?)</', dom, re.S)
    print(f"  page: {counts}")
    print(f"  #status: {re.sub(r'<[^>]+>', ' ', status.group(1)).strip() if status else '?'}")
    bad = [name for name, want in (("CodeMirror", 1), ("thread cells", 8), ("canvas", 1))
           if counts[name] < want]
    if counts["textarea fallback"]:
        bad.append("textarea fallback")
    if bad:
        raise SystemExit(f"tacob-build: the packaged page is wrong: {', '.join(bad)}")
    print(f"\nthe folder at {folder} runs with no Python and no network")


def main():
    ap = argparse.ArgumentParser(prog="tacob-build", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("vendor", help="fetch the page's JavaScript into tools/vendor/")
    p.add_argument("--update", action="store_true",
                   help="accept bytes that differ from the manifest and re-pin them")
    p.set_defaults(fn=cmd_vendor)

    p = sub.add_parser("verify", help="the vendored files against the manifest")
    p.set_defaults(fn=cmd_verify)

    p = sub.add_parser("wine-setup", help="a Wine prefix with a Windows Python + PyInstaller")
    p.set_defaults(fn=cmd_wine_setup)

    p = sub.add_parser("build", help="PyInstaller onedir -> dist/tacob/")
    p.add_argument("--clean", action="store_true", help="remove dist/tacob first")
    p.add_argument("--no-vendor", action="store_true",
                   help="build without the vendored JavaScript (the page then needs a network)")
    p.set_defaults(fn=cmd_build)

    p = sub.add_parser("check", help="run the built folder in a prefix with no Python")
    p.add_argument("--prefix", help="the Python-free prefix to run in")
    p.add_argument("--gamedir", help="the game folder to pass it")
    p.add_argument("--unit", default="armpw")
    p.add_argument("--shot", help="write the page's screenshot here")
    p.add_argument("--online", action="store_true",
                   help="let the page reach the network (the default cuts it off)")
    p.add_argument("args", nargs="*", help="a command line to run instead of the two checks")
    p.set_defaults(fn=cmd_check)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
