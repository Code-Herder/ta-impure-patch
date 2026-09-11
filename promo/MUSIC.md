# The promo's soundtrack

The track is **not in this repository** and must not be added to it: it is licensed
for use *inside* a work, not for redistribution as a file. Everything needed to
prove the licence is here instead, which is also what a platform wants if the video
is ever content-matched.

Render with `--audio <path to the mp3>`; the montage script carries the timing and
the credit, never the path (a path would name a person's home directory).

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

**`start` is 22.0.** That lands the peak at **53.5 s**, on the title card sitting
over the full wall, and the tail resolves at ~55 s with about a second and a half
of quiet before the cut. Verified by measuring the muxed output's RMS per second,
not by listening alone.

The ~22 s of silence before it is **arithmetic, not taste**: 35 s of music cannot
cover 57 s of film *and* finish at the end, and the climax is at the end. The
silence lands on the opening, where one terminal types one command — which plays
better dry, so the constraint and the edit happen to agree.

If a re-cut changes the film's length, move `start` with it:
**`start = duration_of_film − 35.0`**, and re-measure `35.0` if the track changes.

## Rendering with it

```bash
tools/tamontage render promo/tacli-promo.json -o cut.mp4 \
    --backend clip --clips <dir> --audio "<path to the mp3>"
```

The video stream is **copied**, never re-encoded: measured at **1.5 s** for a 57 s
4K cut. That is why the track is chosen *after* the render — auditioning a
candidate costs seconds, so there is no reason to decide one up front. The renderer
prints the required credit every time it muxes, so it cannot be forgotten quietly.
