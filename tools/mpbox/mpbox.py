#!/usr/bin/env python3
"""mpbox -- one isolated box per multiplayer test, so several can run at once.

WHY IT EXISTS. DirectPlay's name server binds UDP 47624 for the whole machine, so two hosting
games collide however separate their wine prefixes are: multiplayer tests have had to queue, one
at a time, and a suite run spends most of its wall clock waiting. A box is a container with
`--network none`, which has a loopback and a port space of its own -- the peers of one test find
each other on 127.0.0.1 and no peer of another test exists to them.

WHAT MAKES IT CHEAP. The repository is bind-mounted **at the same path it has outside**, so every
path an instance already carries -- its prefix, `tacli-state/registry.txt`, the display its
creation pinned -- resolves unchanged, and `tacli` runs *inside* the box. The orchestrator outside
gains a command prefix and nothing else. The image matches the host's Ubuntu and wine exactly,
because a wine of another version would update the bind-mounted prefixes in place.

    mpbox.py build                       # the image, once
    mpbox.py up mp1                      # a box named mp1
    mpbox.py run mp1 -- tools/tacli launch h1 --dplay
    mpbox.py down mp1                    # or `down --all`
    mpbox.py ls

`run` returns the command's own exit code, so a caller can treat it as the command itself.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]                      # the checkout this was run from
IMAGE = "ta-mpbox:1"
PREFIX = "ta-mpbox-"


def mount_root() -> Path:
    """THE MAIN CHECKOUT, not the worktree this runs in: the instances are its
    `tagpu/instances` however many worktrees there are (tacompat's INSTANCES), and every
    linked worktree lives inside it, so one mount at its own path covers both. TACLI_ROOT
    wins where it is set, as it does for tacli itself."""
    if os.environ.get("TACLI_ROOT"):
        return Path(os.environ["TACLI_ROOT"]).expanduser().resolve()
    common = subprocess.run(["git", "rev-parse", "--git-common-dir"], cwd=TREE,
                            capture_output=True, text=True)
    if common.returncode == 0 and common.stdout.strip():
        return Path(common.stdout.strip()).resolve().parent
    return TREE


def docker(*args, **kw):
    return subprocess.run(["docker", *args], **kw)


def box_name(name: str) -> str:
    return name if name.startswith(PREFIX) else PREFIX + name


def cmd_build(args):
    """The image. `--no-cache` when the Dockerfile's packages have to be re-resolved."""
    build = ["build", "-t", IMAGE, "-f", str(HERE / "Dockerfile"), str(HERE)]
    if args.no_cache:
        build.insert(1, "--no-cache")
    return docker(*build).returncode


def cmd_up(args):
    """A box, idle, with the tree mounted at its own path. Already running is not an error:
    a suite that crashed mid-run leaves boxes behind, and re-using one is what a re-run wants."""
    name = box_name(args.name)
    if docker("inspect", "-f", "{{.State.Running}}", name,
              capture_output=True, text=True).stdout.strip() == "true":
        print(name)
        return 0
    docker("rm", "-f", name, capture_output=True)
    # HOME is inside the box (a wine that writes ~/.cache must not touch the owner's), while the
    # tree is the same path on both sides. --network none is the whole point; --shm-size is for
    # the X server, whose default 64 MB is not enough for a 1024x768 game.
    r = docker(
        "run", "-d", "--name", name,
        # AN INIT AS PID 1. `sleep` as PID 1 has no handler for SIGTERM, so every `rm -f` waited
        # out docker's ten seconds and fell through to SIGKILL -- on a daemon other people's
        # containers share, a box must go away the first time it is asked.
        "--init",
        "--network", "none",
        "--shm-size", "512m",
        "--user", f"{os.getuid()}:{os.getgid()}",
        "-e", "HOME=/tmp/boxhome",
        "-e", "WINEDEBUG=-all",
        "-v", f"{mount_root()}:{mount_root()}",
        "-w", str(TREE),
        IMAGE,
    )
    if r.returncode:
        return r.returncode
    # /tmp is the box's own, so every box can hold the same display number and the same wine
    # sockets without seeing another's.
    docker("exec", name, "mkdir", "-p", "/tmp/boxhome", capture_output=True)
    print(name)
    return 0


def cmd_run(args):
    name = box_name(args.name)
    if not args.argv:
        print("mpbox: nothing to run", file=sys.stderr)
        return 2
    return docker("exec", "-w", os.getcwd(), name, *args.argv).returncode


def cmd_down(args):
    names = [box_name(n) for n in args.name]
    if args.all:
        out = docker("ps", "-aq", "--filter", f"name=^{PREFIX}", capture_output=True, text=True)
        names = [n for n in out.stdout.split() if n]
    if not names:
        return 0
    return docker("rm", "-f", *names, capture_output=True).returncode


def cmd_ls(args):
    return docker("ps", "-a", "--filter", f"name=^{PREFIX}",
                  "--format", "{{.Names}}\t{{.Status}}").returncode


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("build", help="build the image")
    p.add_argument("--no-cache", action="store_true")
    p.set_defaults(fn=cmd_build)

    p = sub.add_parser("up", help="start a box")
    p.add_argument("name")
    p.set_defaults(fn=cmd_up)

    p = sub.add_parser("run", help="run a command in a box")
    p.add_argument("name")
    p.add_argument("argv", nargs=argparse.REMAINDER)
    p.set_defaults(fn=cmd_run)

    p = sub.add_parser("down", help="remove boxes")
    p.add_argument("name", nargs="*")
    p.add_argument("--all", action="store_true")
    p.set_defaults(fn=cmd_down)

    p = sub.add_parser("ls", help="list boxes")
    p.set_defaults(fn=cmd_ls)

    args = ap.parse_args()
    if getattr(args, "argv", None) and args.argv and args.argv[0] == "--":
        args.argv = args.argv[1:]
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
