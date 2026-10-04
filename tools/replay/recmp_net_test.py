#!/usr/bin/env python3
"""Run the native isolated-transport fixture in two owned Wine prefixes.

    tools/replay/recmp_net_test.py --port PORT [--out DIR]

PORT is a free DirectPlay discovery port. The output keeps the executable and both
logs. The fixture uses primary-session stand-ins; it does not run TA or prove a
complete recording. It holds session lifetimes by observed application barriers.
"""
import argparse
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

import tareplay

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
OVERRIDES = ("dplayx,dpmodemx,dpnet,dpwsockx,dplaysvr.exe,dpnsvr.exe=n;"
             "dpnhpast,dpnhupnp=d")


def command(args, log, **kwargs):
    subprocess.run(list(map(str, args)), stdout=log, stderr=subprocess.STDOUT,
                   check=True, timeout=180, **kwargs)


def wait_for(jobs, marker):
    deadline = time.monotonic() + 45
    while not all(marker in job[2].read_text(errors="replace") for job in jobs):
        if any(job[0].poll() is not None for job in jobs) or time.monotonic() > deadline:
            raise RuntimeError(f"fixture did not reach {marker}; inspect peer logs")
        time.sleep(.05)


def run(args, output):
    root = tareplay.main_checkout()
    instances = root / "tagpu" / "instances"
    instances.mkdir(parents=True, exist_ok=True)
    # Cooperating fixture runs share a port lease. A foreign holder is never
    # stopped: a native host failure leaves its peer logs for diagnosis.
    with (instances / f"recmp-net-port-{args.port}.lock").open("a") as lease:
        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        display = tareplay.start_xvfb()
        owned, jobs = [], []
        release = output / "release"
        advertise = output / "release.advertise"
        primary = output / "release.primary"
        send = output / "release.send"
        try:
            with (output / "setup.log").open("w") as setup:
                exe = output / "recmp-net-test.exe"
                command(["i686-w64-mingw32-gcc", "-I", TREE / "tagpu/ddraw/inc",
                         "-O2", "-Wall", "-Wextra", "-Werror", "-std=c99", "-o", exe,
                         HERE / "recmp_net_test.c", TREE / "tagpu/ddraw/src/tagpu_recmp_net.c",
                         "-lole32", "-luuid"], setup)
                for role in ("host", "join"):
                    # mkdtemp reserves the directory atomically; cleanup only
                    # reaches directories this invocation actually created.
                    directory = Path(tempfile.mkdtemp(prefix="recmp-net-", dir=instances))
                    prefix = directory / "prefix"
                    env = dict(os.environ, WINEPREFIX=str(prefix), DISPLAY=display.display,
                               WINEDEBUG="-all", WINEDLLOVERRIDES=OVERRIDES)
                    owned.append((directory, env))
                    command([sys.executable, root / "tools/tacli", "create", directory.name,
                             "--private-registry", "--display", display.display,
                             "--res", "640x480"], setup)
                    command(["bash", root / "tools/dpinstall.sh", prefix], setup)
                    command([sys.executable, root / "tools/dpport.py", prefix, args.port], setup)
            for role, (directory, env) in zip(("host", "join"), owned):
                log = output / f"{role}.log"
                handle = log.open("w")
                proc = subprocess.Popen(["wine", str(exe), role,
                                         "Z:" + str(release).replace("/", "\\")],
                                        env=env, stdout=handle, stderr=subprocess.STDOUT)
                jobs.append((proc, handle, log))
                wait_for(jobs, "PRIVATE READY")
                print(f"{role}: private session ready", flush=True)
            # Both primary players exist before either identity advertisement.
            # Both applications consume the identities before either Close.
            advertise.touch()
            wait_for(jobs, "PRIMARY BINDINGS READY")
            primary.touch()
            wait_for(jobs, "PRIMARY CLOSED")
            send.touch()
            wait_for(jobs, "PASS: two primary-native human/AI bindings")
            release.touch()
            for proc, handle, log in jobs:
                if proc.wait(timeout=10):
                    raise RuntimeError(f"fixture failed: {log}")
            print("PASS: both peers bound human/AI sources and delivered after primary Close",
                  flush=True)
        finally:
            for path in (advertise, primary, send, release):
                path.touch()
            # No game is launched in these prefixes. This ends only their own
            # fixture processes and native name servers before removing them.
            for directory, env in owned:
                subprocess.run(["wineserver", "-k"], env=env, check=False, timeout=10)
            for proc, handle, log in jobs:
                try:
                    proc.wait(timeout=10)
                finally:
                    handle.close()
            for directory, env in owned:
                shutil.rmtree(directory)
            tareplay.stop_xvfb(display)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        parser.error("port must be in 1024..65535")
    output = args.out.expanduser().resolve() if args.out else Path(tempfile.mkdtemp(prefix="recmp-net-"))
    if args.out:
        output.mkdir(parents=True, exist_ok=False)
    print(f"evidence: {output}", flush=True)
    try:
        run(args, output)
    except (OSError, RuntimeError, subprocess.SubprocessError, tareplay.CouldNotRun) as error:
        print(f"FAIL: {error}; evidence: {output}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
