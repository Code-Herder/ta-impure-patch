#!/usr/bin/env python3
"""The Wine prefix the wiki's offline checks run their Wine builds in (simparity.mjs's and hostchan.cjs's `wine`
target, textparity.mjs --wine), and the one place it is made: wiki_regression.py imports ensure(), and the node
checks ask this program (tools/wiki/wikibuild.mjs wineEnv).

    python3 tools/wiki/wineprefix.py      the prefix to use, printed once it exists: $WINEPREFIX when it is set and
                                          not refused, else `wikigate-wine` in the repository's common git directory,
                                          made the first time; exit 2, saying why, when it cannot be had

THE INVARIANT: the prefix shares no file with any other prefix. `wineboot -i` makes every file of it from Wine's own
install and reads no other prefix, so nothing the checks run writes into the template prefix `wineprefix/` or
any tacli clone, and no other prefix's wineserver writes into it -- which is also why making it needs no lock. It is made under a name of the making process and renamed
into place only once wineboot's server has exited, so a prefix found at the name is whole; two processes making it at
once both finish, and the one whose rename comes second discards its own. Nothing deletes it: there is one for the
repository, shared by every worktree's checks, and several Wine programs in it at once share its one wineserver, as in
any prefix.

THE REFUSAL: a $WINEPREFIX that is the template, or whose hives are the template's inode (a clone made by a tacli
before clone_prefix copied the hives), is refused rather than run in.

The checks need nothing else of a prefix: their programs are i686 console programs linked -static by the DLL's compiler
(tools/wiki/wikibuild.mjs, textparity.mjs), run with no game file, no DirectPlay and no registry key of tacli's.
"""
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

TREE = Path(__file__).resolve().parents[2]
NAME = "wikigate-wine"
HIVES = ("user.reg", "system.reg", "userdef.reg")
WINEBOOT_S = 600                # a hang bound, deciding nothing: wineboot -i made a prefix in 17-23 s (MEASURED 2026-11-11)
WINESERVER_S = 120              # and on `wineserver -w`, as tacli bounds it (WINESERVER_HANG_S)


class Refused(Exception):
    pass


def common_dir(tree=TREE):
    """The repository's common git directory, the one every worktree of it shares."""
    return Path(subprocess.run(["git", "-C", str(tree), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                               capture_output=True, text=True, check=True).stdout.strip())


def template(tree=TREE):
    """The template prefix every tacli instance is cloned from (tacli TEMPLATE_PREFIX), beside the common git dir."""
    return common_dir(tree).parent / "wineprefix"


def check(prefix, tree=TREE):
    """`prefix`, refused (Refused) when it is the template or one of its hives is a template hive's inode."""
    prefix, tpl = Path(prefix), template(tree)
    if prefix.resolve() == tpl.resolve():
        raise Refused(f"WINEPREFIX {prefix} is the template prefix, whose hives every tacli instance shares")
    for hive in HIVES:
        mine, theirs = prefix / hive, tpl / hive
        if mine.exists() and theirs.exists() and mine.samefile(theirs):
            raise Refused(f"WINEPREFIX {prefix}: its {hive} is the template's own file (a clone made before "
                          f"tacli copied the hives)")
    return prefix


def ensure(tree=TREE):
    """The repository's prefix, made by `wineboot -i` when it does not exist yet (THE INVARIANT)."""
    final = common_dir(tree) / NAME
    if final.is_dir():
        return final
    for old in final.parent.glob(f"{NAME}.making-*"):
        pid = old.name.rsplit("-", 1)[1]
        if pid.isdigit() and not Path(f"/proc/{pid}").exists():
            shutil.rmtree(old, ignore_errors=True)          # a maker that died part-way: its folder is never used
    making = final.with_name(f"{NAME}.making-{os.getpid()}")
    shutil.rmtree(making, ignore_errors=True)
    # no display, so nothing it does can open a window on the desktop; Mono and Gecko declined, whose installers
    # would otherwise be offered: the checks' programs use neither
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update(WINEPREFIX=str(making), WINEDEBUG="-all", WINEDLLOVERRIDES="mscoree,mshtml=")
    print(f"wineprefix: making the wiki checks' Wine prefix {final} (wineboot -i)", file=sys.stderr, flush=True)
    t0 = time.monotonic()
    try:
        subprocess.run(["wineboot", "-i"], env=env, check=True, timeout=WINEBOOT_S, capture_output=True)
        # its server writes the hives when it exits: the prefix is whole once no server of it lives
        subprocess.run(["wineserver", "-w"], env=env, check=True, timeout=WINESERVER_S)
        try:
            making.rename(final)
        except OSError:
            if not final.is_dir():
                raise
            shutil.rmtree(making, ignore_errors=True)       # another process's prefix took the name first
    except BaseException:
        shutil.rmtree(making, ignore_errors=True)
        raise
    print(f"wineprefix: made in {time.monotonic() - t0:.0f} s", file=sys.stderr, flush=True)
    return final


def choose(tree=TREE, environ=os.environ):
    """$WINEPREFIX when it is set, checked; else ensure()'s prefix."""
    given = environ.get("WINEPREFIX")
    return check(given, tree) if given else ensure(tree)


def main():
    try:
        print(choose())
    except (Refused, OSError, subprocess.SubprocessError) as e:
        print(f"wineprefix: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
