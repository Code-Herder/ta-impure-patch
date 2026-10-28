#!/usr/bin/env python3
"""wavtest.py -- tagpu_wav.c on hostile input, and against tools/wiki/gfx.py `wav_pcm`.

    tagpu/ddraw/tests/wavtest.py

Builds tests/wavtest.c natively with the host's gcc, AddressSanitizer and UndefinedBehaviorSanitizer
(tagpu_wav.c is engine-free and Win32-free) and runs its built-in cases: truncated files, huge RIFF,
fmt and data lengths, non-PCM formats, a block align that is not the frame, data before fmt. Then
every WAV the mockup's gitignored packs hold (tools/wiki/mockup/gfx/**/sounds/*.wav, written by
gfx.py from the install) and each built-in case file through both parsers, the C verdict compared
with gfx.py's (accepted or refused alike), every truncation of each parsed under the sanitizers.
Nothing read from the game is written anywhere. Exit 0 when nothing failed."""
import subprocess
import sys
import tempfile
from importlib.machinery import SourceFileLoader
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
ROOT = DDRAW.parent.parent
EXE = HERE / "wavtest-native.exe"       # `.exe` so the tree's *.exe ignore covers it
SRCS = [HERE / "wavtest.c", DDRAW / "src/tagpu_wav.c", DDRAW / "src/tagpu_wav.h"]


def build():
    if EXE.exists() and all(EXE.stat().st_mtime >= s.stat().st_mtime for s in SRCS):
        return
    subprocess.run(["gcc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", f"-I{DDRAW / 'src'}", "-o", str(EXE), str(SRCS[0]), str(SRCS[1])],
                   check=True)


def main():
    build()
    sys.path.insert(0, str(ROOT / "tools/wiki"))
    gfx = SourceFileLoader("gfx", str(ROOT / "tools/wiki/gfx.py")).load_module()
    failed = subprocess.run([str(EXE)]).returncode != 0
    files = sorted((ROOT / "tools/wiki/mockup/gfx").glob("**/sounds/*.wav"))
    with tempfile.TemporaryDirectory() as tmp:
        # gfx.py's twin cases: the C side's own files are made in C, so these are made here the same way
        import struct
        def make(riff, fmtlen, tag, ch, rate, align, bits, datalen, have):
            return (b"RIFF" + struct.pack("<I", riff) + b"WAVE" + b"fmt " + struct.pack("<IHHIIHH", fmtlen, tag, ch, rate,
                    (rate * align) & 0xFFFFFFFF, align, bits) + b"data" + struct.pack("<I", datalen) + b"\x80" * have)
        cases = [make(136, 16, 1, 1, 22050, 1, 8, 100, 100), make(138, 16, 1, 2, 44100, 4, 16, 102, 102),
                 make(136, 16, 2, 1, 22050, 256, 4, 100, 100), make(0xFFFFFFFF, 16, 1, 1, 22050, 1, 8, 0xFFFFFFF0, 100),
                 make(136, 0xFFFFFFF0, 1, 1, 22050, 1, 8, 100, 100), make(20, 16, 1, 1, 22050, 1, 8, 100, 100),
                 make(136, 16, 1, 1, 22050, 1, 8, 100, 0), make(136, 16, 1, 1, 0xFFFFFFFF, 1, 8, 100, 100)]
        for i, c in enumerate(cases):
            p = Path(tmp) / f"case{i}.wav"
            p.write_bytes(c)
            files.append(p)
        out = subprocess.run([str(EXE), *map(str, files)], capture_output=True, text=True)
        lines = [l for l in out.stdout.splitlines() if not l.startswith("FAIL")]
        for l in out.stdout.splitlines():
            if l.startswith("FAIL"):
                print(l)
                failed = True
        if out.returncode != 0 or len(lines) != len(files):
            print(f"FAIL the file pass: exit {out.returncode}, {len(lines)} verdicts for {len(files)} files\n{out.stderr}")
            failed = True
        agree = 0
        for f, l in zip(files, lines):
            py = gfx.wav_pcm(f.read_bytes())
            if (py is None) != l.startswith("ok "):
                print(f"FAIL {f.name}: C {l}, gfx.py {py or 'accepted'}")
                failed = True
            else:
                agree += 1
        print(f"{agree} of {len(files)} files ({len(files) - len(cases)} from the packs) judged alike by C and gfx.py")
    print("FAILED" if failed else "passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
