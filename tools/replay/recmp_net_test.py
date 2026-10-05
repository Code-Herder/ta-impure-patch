#!/usr/bin/env python3
"""Run the native isolated-transport fixture in two owned Wine prefixes.

    tools/replay/recmp_net_test.py --port PORT [--out DIR] [--channel]

PORT is a free DirectPlay discovery port. The output keeps the executable and both
logs. The fixture uses primary-session stand-ins; it does not run TA or prove a
complete recording. --channel starts the production channel after both primary
interfaces close; the default tests the backend directly. Both run authentication
and controlled channel tests. Session lifetimes use observed test barriers.
"""
import argparse
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import sys
import struct
import tempfile
import time
import zlib

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


def check_journal(raw):
    """Validate persisted packet history independently of the C queue/ledger."""
    if len(raw) < 112:
        raise ValueError("journal header cut short")
    version, count, recorder, reserved, match = struct.unpack_from("<IIII16s", raw)
    if version != 1 or not 1 <= count <= 10 or reserved or not any(match):
        raise ValueError("journal header refused")
    actors = dict(struct.unpack_from("<II", raw, 32+8*i) for i in range(count))
    if len(actors) != count or any(not a or role not in (0, 1) for a, role in actors.items()) or actors.get(recorder) != 1:
        raise ValueError("journal roster refused")
    ledger = {a: dict(sequence=0, tick=0, size=0, crc=0, closed=None, ended=False, stream=None) for a in actors}
    at, ordinal, application_last = 112, 0, 0
    while at < len(raw):
        if len(raw)-at < 80:
            raise ValueError("journal record cut short")
        order, application_tick, phase = struct.unpack_from("<QII", raw, at)
        at += 16
        values = struct.unpack_from("<8sHHIIIII16s16s", raw, at)
        magic, version, header, size, origin, tick, sequence, kind, packet_match, stream = values
        if (order != ordinal+1 or application_tick < application_last or origin not in actors or phase != (1 if actors[origin] else 2) or
                magic != b"IMPUREMP" or version != 2 or header != 64 or not 64 <= size <= 4096 or
                size > len(raw)-at or packet_match != match or not any(stream)):
            raise ValueError("journal packet metadata refused")
        s = ledger[origin]
        if (s["ended"] or sequence != s["sequence"]+1 or tick < s["tick"] or
                (s["stream"] is not None and stream != s["stream"])):
            raise ValueError("journal source order refused")
        if kind == 4:
            if size != 88:
                raise ValueError("journal seal span refused")
            previous, closed, data_bytes, crc, final = struct.unpack_from("<IIQII", raw, at+64)
            if ((previous, closed, data_bytes, crc) != (s["sequence"], tick, s["size"], s["crc"]) or
                    final not in (0, 1) or (s["closed"] is not None and
                    (tick < s["closed"] or (not final and tick == s["closed"])))):
                raise ValueError("journal seal refused")
            s["closed"], s["ended"] = tick, bool(final)
        elif kind in (1, 2, 6, 7):
            if s["closed"] is not None and tick <= s["closed"]:
                raise ValueError("journal closed tick refused")
            s["size"] += size
            s["crc"] = zlib.crc32(raw[at:at+size], s["crc"])
        else:
            raise ValueError("journal packet kind refused")
        s["sequence"], s["tick"], s["stream"] = sequence, tick, stream
        ordinal, at, application_last = order, at+size, application_tick
    if ordinal != 40 or any(s["sequence"] != 10 or not s["ended"] for s in ledger.values()):
        raise ValueError("journal final prefixes missing")
    return ordinal


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
                         *(["-DRECMP_CHANNEL_TEST", TREE / "tagpu/ddraw/src/tagpu_recmp.c"] if args.channel else []),
                         HERE / "recmp_net_test.c", TREE / "tagpu/ddraw/src/tagpu_recmp_net.c",
                         TREE / "tagpu/ddraw/src/tagpu_recmp_auth.c",
                         TREE / "tagpu/ddraw/src/tagpu_recmp_record.c",
                         TREE / "tagpu/ddraw/src/tagpu_recmp_stream.c", TREE / "tagpu/ddraw/src/crc32.c",
                         "-lole32", "-luuid"], setup)
                auth = output / "recmp-auth-test.exe"
                command(["i686-w64-mingw32-gcc", "-I", TREE / "tagpu/ddraw/inc",
                         "-O2", "-Wall", "-Wextra", "-Werror", "-std=c99", "-o", auth,
                         HERE / "recmp_auth_test.c", TREE / "tagpu/ddraw/src/tagpu_recmp_net.c",
                         TREE / "tagpu/ddraw/src/tagpu_recmp_auth.c", "-lole32", "-luuid"], setup)
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
            with (output / "auth.log").open("w") as log:
                command(["wine", auth], log, env=owned[0][1])
            print("authentication bounds and forgery controls: PASS", flush=True)
            with (output / "setup.log").open("a") as setup:
                controls = output / "recmp-transport-test.exe"
                command(["i686-w64-mingw32-gcc", "-I", TREE / "tagpu/ddraw/inc",
                         "-O2", "-Wall", "-Wextra", "-Werror", "-std=c99", "-o", controls,
                         HERE / "recmp_transport_test.c", TREE / "tagpu/ddraw/src/tagpu_recmp.c",
                         TREE / "tagpu/ddraw/src/tagpu_recmp_auth.c",
                         TREE / "tagpu/ddraw/src/tagpu_recmp_record.c",
                         TREE / "tagpu/ddraw/src/tagpu_recmp_stream.c", TREE / "tagpu/ddraw/src/crc32.c",
                         "-lole32", "-luuid"], setup)
                streams = output / "recmp-stream-test.exe"
                command(["i686-w64-mingw32-gcc", "-I", TREE / "tagpu/ddraw/inc",
                         "-O2", "-Wall", "-Wextra", "-Werror", "-std=c99", "-o", streams,
                         HERE / "recmp_stream_test.c", TREE / "tagpu/ddraw/src/tagpu_recmp_stream.c",
                         TREE / "tagpu/ddraw/src/crc32.c"], setup)
                cuts = output / "recmp-cut-test.exe"
                command(["i686-w64-mingw32-gcc", "-I", TREE / "tagpu/ddraw/inc",
                         "-O2", "-Wall", "-Wextra", "-Werror", "-std=c99", "-o", cuts,
                         HERE / "recmp_cut_test.c", TREE / "tagpu/ddraw/src/tagpu_recmp.c",
                         TREE / "tagpu/ddraw/src/tagpu_recmp_auth.c",
                         TREE / "tagpu/ddraw/src/tagpu_recmp_record.c",
                         TREE / "tagpu/ddraw/src/tagpu_recmp_stream.c", TREE / "tagpu/ddraw/src/crc32.c",
                         "-lole32", "-luuid"], setup)
                journal = output / "recmp-record-test.exe"
                command(["i686-w64-mingw32-gcc", "-I", TREE / "tagpu/ddraw/inc",
                         "-O2", "-Wall", "-Wextra", "-Werror", "-std=c99", "-o", journal,
                         HERE / "recmp_record_test.c", TREE / "tagpu/ddraw/src/tagpu_recmp_record.c"], setup)
            with (output / "stream.log").open("w") as log:
                command(["wine", streams], log, env=owned[0][1])
            with (output / "journal.log").open("w") as log:
                command(["wine", journal], log, env=owned[0][1])
            for mode in ("", "gap", "unauthorized", "tail", "missing_end"):
                with (output / ("channel-" + (mode or "bounds") + ".log")).open("w") as log:
                    command(["wine", controls, *([mode] if mode else [])], log, env=owned[0][1])
            print("source-prefix ledger, private channel bounds/gap/wrong-source/missing-tail and abandoned-drain controls: PASS", flush=True)
            for mode in ("", "missing_receipt", "missing_cut", "bad_receipt", "journal_full"):
                with (output / ("cut-" + (mode or "handoff") + ".log")).open("w") as log:
                    command(["wine", cuts, *([mode] if mode else [])], log, env=owned[0][1])
            print("consumption cuts, terminal receipts and retained tail controls: PASS", flush=True)
            for role, (directory, env) in zip(("host", "join"), owned):
                log = output / f"{role}.log"
                handle = log.open("w")
                proc = subprocess.Popen(["wine", str(exe), role,
                                         "Z:" + str(release).replace("/", "\\")],
                                        env=env, stdout=handle, stderr=subprocess.STDOUT)
                jobs.append((proc, handle, log))
                wait_for(jobs, "PRIMARY READY" if args.channel else "PRIVATE READY")
                print(f"{role}: primary players ready" if args.channel else f"{role}: private session ready", flush=True)
            # Both primary players exist before native player-key reads.
            # Both applications copy native player keys before either Close.
            advertise.touch()
            wait_for(jobs, "PRIMARY KEYS READY")
            primary.touch()
            wait_for(jobs, "PRIMARY CLOSED")
            send.touch()
            wait_for(jobs, "PASS: production channel after primary Close" if args.channel else
                     "PASS: two primary-native human/AI keys")
            release.touch()
            for proc, handle, log in jobs:
                if proc.wait(timeout=10):
                    raise RuntimeError(f"fixture failed: {log}")
            if args.channel:
                for role in ("host", "join"):
                    raw = (output / f"release.{role}.journal").read_bytes()
                    if len(raw) != 4016 or check_journal(raw) != 40:
                        raise RuntimeError("persisted journal mismatch")
                    for altered in (raw[:-1], raw[:112+16+64] + bytes([raw[112+16+64] ^ 1]) + raw[112+16+65:]):
                        try:
                            check_journal(altered)
                        except ValueError:
                            pass
                        else:
                            raise RuntimeError("persisted journal corruption accepted")
                print("persisted journal round trip and corruption/missing-tail controls: PASS", flush=True)
            print("PASS: both peers verified signed human/AI sources and delivered after primary Close" +
                  (" through the production channel" if args.channel else ""),
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
    parser.add_argument("--channel", action="store_true",
                        help="test the production channel with both primary interfaces already closed")
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
