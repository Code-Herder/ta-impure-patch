# The promo's audio

Two recordings: the score, and a keyboard. **Neither is in this repository and
neither may be added to it** — both are licensed for use *inside* a work rather
than for redistribution as a file. Everything needed to prove the licence is here
instead, which is also what a platform wants if the video is ever content-matched.

Render with `--audio <mp3>` and `--keys <mp3>`; the montage script carries the
timing, the splice recipe and the credit, never the paths (a path would name a
person's home directory).

## Evening Melodrama

> "Theme for a 1980's-esque prime time TV melodrama."

| | |
|---|---|
| **Title** | Evening Melodrama |
| **Artist** | Kevin MacLeod |
| **Source** | <https://incompetech.com/music/royalty-free/music.html> |
| **Licence** | Creative Commons Attribution 4.0 (CC BY 4.0) |
| **Licence URL** | <http://creativecommons.org/licenses/by/4.0/> |
| **ISRC** | USUAN1200049 |
| **Album** | Misc (Film Scoring Moods) |
| **Genre** | Soundtrack |
| **Feel** | Bouncy, Bright, Epic, Uplifting |
| **Tempo** | 115 bpm |
| **Listed length** | 0:36 |
| **Actual file** | **40.18 s**, 320 kbps, 44.1 kHz stereo — measured, and it is what the edit is cut to |
| **Instruments** | French Horn, Trombones, Trumpet, Triangle, Violins, Cellos, Basses, Tympani, Snare, Flute, Clarinet |
| **Uploaded** | 2012-04-14 |

## Attribution — required, and this is the wording

CC BY 4.0 obliges us to credit the author. Incompetech's own required form:

```
"Evening Melodrama" Kevin MacLeod (incompetech.com)
Licensed under Creative Commons: By Attribution 4.0 License
http://creativecommons.org/licenses/by/4.0/
```

Put it where someone who wants to know where the music came from will find it
without difficulty, and do not obscure it: the video description wherever the film
is posted, and the README section that embeds the film. A credit that exists only
in this file does not discharge the licence.

## If the video is content-matched

Kevin MacLeod's catalogue is widely registered with content-ID systems, so a match
is an expected event, not a sign anything is wrong. The dispute answer is: the work
is licensed CC BY 4.0 by the composer for exactly this use, the attribution is in
the description, and the recording is ISRC **USUAN1200049** from
incompetech.com. Keep this file's facts and the date the file was downloaded.

## How it is cut to the film

**Align the music's hit to the film's hit, never the two files' ends.** The mp3 is
40.18 s long but roughly **5 s of that is digital silence** — the music peaks at
**31.5 s** and its tail resolves by about **35 s**, which is why the source page
lists the track as 0:36. Measured, not assumed:

| track time | level |
|---|---|
| 31-32 s | peak |
| 32-33 s | ~half |
| 33-34 s | tail |
| 35 s on | inaudible, then digital silence to 40.18 s |

The first attempt here set `start = 57.0 − 40.18 = 16.82` from the file length,
which put the peak at 48 s and left the climax — the 1600-window wide shot at 51 s
and the title card at 51.5 s — playing over **dead air**. The bug was using the
file's duration for the music's duration.

**`start` is 22.0, and it stays 22.0** — it is authored against the *unlooped*
track. That lands the climax at **47 s**, measured as the loudest second at
−13.0 dBFS, with the tail resolving by ~55 s.

### The 22 s of silence was arithmetic; the loop is how we bought it back

35 s of music cannot cover 57 s of film *and* finish at the end, and the climax
has to be at the end — so the opening was silent to 22 s. The owner's call was
that this was too long, with one constraint: **the typing stays dry, and the
music comes in on the first zoom-out** (7.47 s).

The only way to start earlier without moving the climax is to make the track
*longer at the front*. `audio.loop` in the montage script does it with **one
join**, in the flat body, under a one-beat crossfade:

```json
"loop": { "crossfade_ms": 500, "segments": [[0.0, 21.48], [9.018, null]] }
```

Play 0 → 21.48, jump back six bars to 9.018, play on to the end: **+11.96 s**
after the fade. That alone reaches 9.6 s, so `start` is authored **19.9** rather
than 22.0 — the whole cue sits 2.1 s earlier and the music enters at **7.94 s**.
The price is that the tail goes quiet at 53 s instead of 55; the loudest second
moves from 47 s to 45 s. One seam was worth it.

Measured on the built file: join at 29.4 s steps **−0.9 dB**; typing 1–3 s dry;
music audible from 8 s.

**Four plans were built and rejected by ear before this one.** The whole record —
what was tried, why each failed, and what the measurements did and did not
predict — is in the ta-video-montage skill under *Stretching music for a video*.
`promo/loop-finder.py --from-max 30` reproduces the search. Do not change these
numbers without reading that section: this cue is not loop music, and the seam
is hidden by the long fade and the single join, not by alignment.

If a re-cut changes the film's length, move `start` with it:
**`start = duration_of_film − 35.0`** (still against the unlooped track), and
re-measure `35.0` if the track changes.

## Rendering with it

```bash
tools/tamontage render promo/tacli-promo.json -o cut.mp4 \
    --backend clip --clips <dir> \
    --audio "<path to the mp3>" --keys "<path to the typing recording>"
```

## The typing

`--keys` supplies a recording of someone typing; it is sliced into individual
one-shots at render time and one is placed on each keystroke (see
`audio.keyboard` in the montage script). Same rule as the music: the path is an
argument, never the script, and the file never enters this repository.

| | |
|---|---|
| **Recording** | "Fast Typing on Mechanical Keyboard" |
| **Uploader** | freesound_community, via Pixabay |
| **Source** | <https://pixabay.com/sound-effects/film-special-effects-fast-typing-on-mechanical-keyboard-28197/> |
| **Licence** | Pixabay Content Licence — **no attribution required**, commercial use allowed |
| **Prohibition that applies** | may not be sold or distributed "on a Standalone basis", i.e. unchanged and on its own. Cut into one-shots and mixed under a film is not that. |
| **Switches** | Kailh Blue (the upload is tagged "Blue, Havit, Kailh") — a genuinely clicky board |
| **File** | 36.1 s, 24 kHz, 160 kbps, stereo, 706 KB |
| **Downloaded** | 2026-09-11 |

The fidelity is the weak point — 24 kHz against the 48 kHz/320 kbps CC0 takes on
BigSoundBank — but those are membrane boards, and the owner chose the sound over
the spec sheet.

**Synthesised clicks were built first and rejected.** They cost nothing and carry
no licence, and they did not sound good; the only test that settled it was the
owner listening to three of them. That code is gone — `git log` has it.

The video stream is **copied**, never re-encoded: measured at **0.5 s** for the
57 s / 227 MB 4K cut. That is why the track is chosen *after* the render — auditioning a
candidate costs seconds, so there is no reason to decide one up front. The renderer
prints the required credit every time it muxes, so it cannot be forgotten quietly.
