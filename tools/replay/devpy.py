"""devpy -- run a replay tool under the project's Python environment when it needs one.

    import devpy; devpy.ensure()        # at the top of a tool's main()

A tool that reads a recording's blocks needs numpy, numba and zstandard (reccodec.py). The system
python3 has none of them, and the shared venv at the main checkout (CLAUDE.md, *Python tooling*)
has all three: ensure() returns at once when they are there, else re-executes this process under
the venv's interpreter with the same arguments, found the way research/build_wiki.py's
find_python finds it (this checkout's root, then the main checkout of a worktree). An environment
variable stops a second re-execution, so a venv that lacks them fails here, by name, instead of
looping.

Standard library only: it runs under whichever python3 started the tool.
"""
import importlib.util
import os
import subprocess
import sys
from pathlib import Path

GUARD = "TAGPU_DEVPY_REEXEC"
REPO = Path(__file__).resolve().parents[2]


def _have():
    try:
        import numpy  # noqa: F401
        import zstandard  # noqa: F401
    except ImportError:
        return False
    # looked up, not imported: importing numba costs a tenth of a second, which reccodec.py pays
    # only when it decodes a sectioned block
    return importlib.util.find_spec("numba") is not None


def venv_python():
    cands = [REPO / ".venv-undither" / "bin" / "python"]
    try:
        common = subprocess.run(["git", "rev-parse", "--git-common-dir"], capture_output=True,
                                text=True, check=True, cwd=REPO).stdout.strip()
        main_root = (Path(common) if Path(common).is_absolute() else REPO / common).resolve().parent
        cands.append(main_root / ".venv-undither" / "bin" / "python")
    except (OSError, subprocess.CalledProcessError):
        pass
    return next((str(c) for c in cands if c.exists()), None)


def ensure():
    if _have():
        return
    py = None if os.environ.get(GUARD) else venv_python()
    if py is None:
        print("needs numpy, numba and zstandard: the shared venv (CLAUDE.md, Python tooling)",
              file=sys.stderr)
        sys.exit(2)
    os.environ[GUARD] = "1"
    os.execv(py, [py] + sys.argv)
