#!/usr/bin/env python3
"""tacob_app.py — the packaged tool's entry point (research/notes/tacob-design.md §Landing 5).

`tools/tacob`, `tools/ta3do` and `tools/hpipack.py` are the whole program, and the
first two are extensionless CLI scripts that the third is loaded beside — all three
reached by *file path* through `SourceFileLoader`, never imported by name. That is
precisely what PyInstaller's import analysis cannot see through, so this file does
two things and nothing else:

  * it imports, by name, every standard-library module those three reach for, so
    the analysis puts them in the bundle. The list below is not decoration: drop
    one and the packaged folder starts and then fails on the route that needed it.
    It was taken from their own `import` statements (an AST walk, not memory) and
    `tacob-build.py check` is what proves it is still complete.
  * it loads `tacob` out of the bundle and hands it the command line — defaulting
    to `gui`, because the packaged folder is something a modder double-clicks and
    the window is the front door.

Run from a checkout it behaves the same way, which is how the launcher is
exercised without building anything: `python3 tools/tacob_app.py --no-open`.
"""

# The standard library `tacob`, `ta3do` and `hpipack` use. Alphabetical, and every
# name here appears in one of their `import` lines.
import argparse            # noqa: F401  the CLI
import contextlib          # noqa: F401  Session._load_model, capturing ta3do's stderr
import difflib             # noqa: F401  the run gate's trace diff
import http.server         # noqa: F401  the editor's server
import importlib.machinery  # noqa: F401  SourceFileLoader — how the three find each other
import importlib.util      # noqa: F401
import io                  # noqa: F401
import json                # noqa: F401
import math                # noqa: F401
import os                  # noqa: F401
import pathlib             # noqa: F401
import re                  # noqa: F401
import shutil              # noqa: F401  ta3do.Display, find_chrome
import struct              # noqa: F401  every format this reads
import subprocess          # noqa: F401  ta3do's undither shell-out (the editor never reaches it)
import sys
import tempfile            # noqa: F401
import threading           # noqa: F401  the tick loop
import time                # noqa: F401
import urllib.parse        # noqa: F401
import webbrowser          # noqa: F401  the launcher's fallback
import zlib                # noqa: F401  inflate, and writing PNGs

if sys.platform == "win32":
    import winreg          # noqa: F401  the WebView2 probe


def load_tacob():
    """`tacob` out of the folder this was frozen into, or out of `tools/` when it
    was not frozen at all."""
    from importlib.machinery import SourceFileLoader
    import importlib.util
    here = getattr(sys, "_MEIPASS", None)
    if here is None:
        here = pathlib.Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) \
            else pathlib.Path(__file__).resolve().parent
    loader = SourceFileLoader("tacob", str(pathlib.Path(here) / "tacob"))
    spec = importlib.util.spec_from_loader("tacob", loader)
    module = importlib.util.module_from_spec(spec)
    sys.modules["tacob"] = module
    loader.exec_module(module)
    return module


def command_line(argv):
    """`gui` is the default subcommand.

    `tacob.exe` on its own is a double-click and must open the window, and
    `tacob.exe --no-open` is that window's own flag — but `--help` stays the whole
    tool's help, because that is the one a modder types to find the rest of it."""
    argv = list(argv)
    if not argv or (argv[0].startswith("-") and argv[0] not in ("-h", "--help")):
        return ["gui"] + argv
    return argv


def main():
    # The console this lands in is whatever Windows gives it, and when its output
    # is redirected Python encodes with the machine's legacy code page — where a
    # single em dash in a message is a `UnicodeEncodeError` and the tool dies
    # printing its own greeting. Ask for UTF-8 and never fail on a character.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError, OSError):
            pass
    load_tacob().main(command_line(sys.argv[1:]))


if __name__ == "__main__":
    main()
