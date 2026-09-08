# -*- mode: python ; coding: utf-8 -*-
"""PyInstaller spec for tacob — the Windows folder that needs no Python (landing 5).

    tools/tacob-build.py build          # what actually runs this, under Wine

**onedir, not onefile** (`tacob-design.md` §"Decisions locked", "The exe"): a
folder a modder can look inside, whose `tacob-edit.html` and `tacob-include/*.h`
are editable in place, and which starts without unpacking 40 MB to a temporary
directory on every run.

Everything the program *is* ships as **data**, not as code: `tacob`, `ta3do` and
`hpipack.py` are extensionless CLI scripts that find each other by file path
(`SourceFileLoader`), and `tacob._here()` resolves that path to `sys._MEIPASS`
when frozen — which is this spec's `.` destination. `tools/tacob_app.py` is the
only real entry point, and its import block is what puts the standard library
those three use into the bundle.

Paths are derived from `SPECPATH`, never written down: nothing about the
build host belongs in a tracked file.
"""

from pathlib import Path

TOOLS = Path(SPECPATH)                                   # noqa: F821 — PyInstaller's global
ROOT = TOOLS.parent

# `.` is `sys._MEIPASS` at run time — the directory `tacob._here()` returns.
datas = [
    (str(TOOLS / "tacob"), "."),                 # the tool
    (str(TOOLS / "ta3do"), "."),                 # the asset layer it loads by path
    (str(TOOLS / "hpipack.py"), "."),            # what `pack` writes the .ufo with
    (str(TOOLS / "tacob-edit.html"), "."),       # the editor page
    (str(TOOLS / "tacob-setup.html"), "."),      # the first-run picker
    (str(TOOLS / "tacob-include"), "tacob-include"),   # the shipped BOS headers
]

# The pinned JavaScript, when `tacob-build.py vendor` has fetched it. Without it
# the page still loads — from the CDN, if the machine has a network — so this is
# a warning at build time, not an error.
VENDOR = TOOLS / "vendor"
if VENDOR.is_dir():
    datas.append((str(VENDOR), "vendor"))
    datas.append((str(TOOLS / "tacob-vendor.json"), "."))   # what those bytes are
else:
    print("tacob.spec: no tools/vendor/ — the packaged page will need a network")

a = Analysis(                                            # noqa: F821
    [str(TOOLS / "tacob_app.py")],
    pathex=[str(TOOLS)],
    binaries=[],
    datas=datas,
    # pywebview is optional at run time (`tacob.open_window` falls back to the
    # browser), but when it is installed in the build environment we want its
    # Windows backend in the folder — that is the window a modder gets.
    hiddenimports=["webview", "webview.platforms.winforms", "clr_loader"],
    hookspath=[],
    runtime_hooks=[],
    excludes=["tkinter", "numpy", "PIL", "matplotlib", "pytest", "test"],
    noarchive=False,
)
pyz = PYZ(a.pure)                                        # noqa: F821

exe = EXE(                                               # noqa: F821
    pyz, a.scripts, [],
    exclude_binaries=True,
    name="tacob",
    debug=False,
    strip=False,
    upx=False,
    # A console, deliberately. The browser fallback has no window of its own to
    # close, so this is the one that stops the server — and it is where the URL,
    # the lint findings and any traceback appear.
    console=True,
)
coll = COLLECT(                                          # noqa: F821
    exe, a.binaries, a.datas,
    strip=False, upx=False,
    name="tacob",
)
