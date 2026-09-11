#!/usr/bin/env python3
"""Where can a music cue be extended by looping, and how badly?

    promo/loop-finder.py <track.mp3> [--body 8 30] [--spans 4 6 7 8]

Prints the per-bar level profile (loop where it is FLAT, never on a crescendo),
the bar length measured from the track itself (never the stated bpm: 115 vs
115.85 is a 37 ms stutter over four bars), and single-jump candidates scored on
harmonic match (chroma) and level continuity. It does not choose for you: on a
through-composed cue no join is inaudible, and the owner's ear is the gate.
See the ta-video-montage skill, "Stretching music for a video".

numpy only; the renderer's interpreter has no scipy.
"""
import argparse, subprocess, sys
import numpy as np

SR, HOP, N_FFT = 22050, 512, 4096


def load(path):
    o = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "f32le",
                        "-ac", "1", "-ar", str(SR), "-"], capture_output=True)
    x = np.frombuffer(o.stdout, np.float32).astype(np.float64)
    if x.size == 0:
        raise SystemExit(f"{path}: ffmpeg decoded nothing")
    return x


def chroma(x):
    """12 pitch classes per hop, mean-removed and unit-norm: what is DISTINCTIVE
    about this moment's harmony. A spectral envelope scores every bar of an
    orchestral piece at ~0.997 against every other -- it says only 'orchestra'."""
    w = np.hanning(N_FFT)
    frames = 1 + (len(x) - N_FFT) // HOP
    f = np.fft.rfftfreq(N_FFT, 1 / SR)
    ok = f > 55.0
    pc = np.zeros(len(f), int)
    pc[ok] = np.round(12 * np.log2(f[ok] / 440.0)).astype(int) % 12
    C = np.zeros((frames, 12))
    for i in range(frames):
        S = np.abs(np.fft.rfft(x[i * HOP:i * HOP + N_FFT] * w))
        S[~ok] = 0
        for b in range(12):
            C[i, b] = S[pc == b].sum()
    C = np.log1p(C * 20)
    C -= C.mean(axis=1, keepdims=True)
    return C / np.maximum(np.linalg.norm(C, axis=1, keepdims=True), 1e-9)


def bar_length(C, t0, t1, lo=1.5, hi=2.6):
    """Repeat period of the HARMONY over the body: the lag at which chroma frames
    best match themselves, parabolic-refined.

    Not the onset envelope. That autocorrelation has several peaks between 2.0
    and 2.1 s on an orchestral cue and the search window decides which wins --
    it returned 2.0198 and 2.0717 on the same track depending on the window.
    Chroma gave 2.07 both times and agreed with the stated 115 bpm to 0.7 %."""
    fps = SR / HOP
    a, b = int(t0 * fps), int(t1 * fps)
    lags = np.arange(int(lo * fps), int(hi * fps))
    sc = np.array([float((C[a:b - L] * C[a + L:b]).sum() / (b - L - a)) for L in lags])
    j = int(np.argmax(sc))
    if 0 < j < len(sc) - 1:
        y0, y1, y2 = sc[j - 1], sc[j], sc[j + 1]
        d = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2) if (y0 - 2 * y1 + y2) else 0.0
    else:
        d = 0.0
    return (lags[j] + d) / fps


def rms_db(x, t0, t1):
    s = x[int(t0 * SR):int(t1 * SR)]
    return 20 * np.log10(max(float(np.sqrt((s ** 2).mean())), 1e-9))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("track")
    ap.add_argument("--body", nargs=2, type=float, default=(8.0, 20.0),
                    help="where the jump may LAND (the flat region), in seconds")
    ap.add_argument("--spans", nargs="+", type=int, default=(4, 6, 7, 8),
                    help="loop lengths to try, in bars")
    ap.add_argument("--bar", type=float, help="bar length in s (default: measured)")
    ap.add_argument("--flat", type=float, default=2.0,
                    help="dB tolerance that defines the flat body (default 2)")
    ap.add_argument("--from-max", type=float,
                    help="latest allowed `from` point, in s. Cap it BEFORE the run-up "
                         "to the climax: that passage is as loud as the body, so the "
                         "level filter cannot see it, and jumping out of it plays the "
                         "climax twice. The loudest bar is printed as a hint")
    a = ap.parse_args()

    x = load(a.track)
    C = chroma(x)
    bar = a.bar or bar_length(C, *a.body)
    print(f"bar {bar:.4f} s  ({240 / bar:.2f} bpm, measured from the harmony -- "
          f"never the stated tempo)")

    print("\nlevel per bar (loop only where this is FLAT):")
    prev = None
    t = 0.3
    while t + bar < len(x) / SR:
        v = rms_db(x, t, t + bar)
        print(f"  {t:6.2f}s  {v:6.1f} dBFS" + (f"  ({v - prev:+.1f})" if prev is not None else ""))
        prev, t = v, t + bar

    fps = SR / HOP

    def join(F, T, win=2.0):
        w = int(win * fps)
        iF, iT = int(F * fps), int(T * fps)
        return -9.0 if iF - w < 0 or iT > len(C) else float((C[iF - w:iF] * C[iT - w:iT]).sum() / w)

    # BOTH ends of a jump must sit in the flat region: a jump out of the build
    # to the climax scores well on chroma and plays the climax twice. Flat is
    # within `--flat` dB of the median bar level over --body.
    levels = []
    t = 0.3
    while t + bar < len(x) / SR:
        levels.append((t, rms_db(x, t, t + bar)))
        t += bar
    med = float(np.median([v for t, v in levels if a.body[0] <= t < a.body[1]]))
    flat = lambda t: abs(rms_db(x, max(0, t - 0.5), t + 0.5) - med) <= a.flat
    print(f"\nflat body: bars within {a.flat:g} dB of {med:.1f} dBFS; both ends of a jump must be in it")
    if a.from_max is None:
        print("no --from-max: candidates may jump out of the run-up to the climax, which is as")
        print("loud as the body and so invisible to the level filter. Cap it by ear.")

    # Top THREE per span, not the best: the scores sit within +-0.02 of each
    # other across the body, so the single maximum is chosen on noise. The tool
    # proposes a short list; an ear picks. That is how the shipped loop was found.
    print("\nsingle-jump candidates (play 0->from, jump to `to`, play on). ONE join;")
    print("level step should be ~0 dB; chroma above ~0.75 is worth auditioning:")
    for bars in a.spans:
        span = bars * bar
        cands = []
        for T in np.arange(a.body[0], a.body[1], 0.05):
            F = T + span
            if F > len(x) / SR - 5:
                break
            if not (flat(T) and flat(F)):
                continue
            if a.from_max is not None and F > a.from_max:
                break
            step = rms_db(x, T, T + 0.5) - rms_db(x, F - 0.5, F)
            if abs(step) > 1.5:
                continue
            cands.append((join(F, T), F, T, step))
        cands.sort(reverse=True)
        shown = []
        for sc, F, T, step in cands:
            if any(abs(F - g) < 1.0 for g in shown):
                continue
            shown.append(F)
            print(f"  {bars} bars: from {F:6.2f} -> to {T:6.2f}   chroma {sc:+.3f}   "
                  f"level step {step:+.1f} dB   adds {span:5.2f} s")
            if len(shown) == 3:
                break
    print("\nthen: crossfade one BEAT (~500 ms), never 18 ms; verify the level step on the")
    print("built file; and LISTEN -- on a through-composed cue no metric replaces that.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
