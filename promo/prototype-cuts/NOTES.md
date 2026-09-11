# PROTOTYPE — five cuts of the tacli promo

**Throwaway.** Delete this directory once a cut is chosen; fold the answer into
`promo/tacli-promo.json` and the `ta-video-montage` skill first.

## The question

We built one cut straight from the brief (one terminal → four → a 40-wide wall) and it
looks fine — but "looks fine" is what you say about the only option you have seen.
**What should the promo actually look like?** Four rivals, each disagreeing with the
brief on exactly one axis, so the choice gets made by watching instead of imagining.

## Run it

```bash
promo/prototype-cuts/render.sh [outdir]     # ~2.5 min, five cuts + a compare page
```

Renders at 960×540 — this is for judging **pacing**, not pixels. Opens as
`<outdir>/index.html`: bottom bar or keys `1`–`5` to switch, playback position is
preserved across switches so the same moment can be compared cut to cut.

## The five

| id | axis it argues | what it changes |
|---|---|---|
| `a-ladder` | *(control)* | the brief as built: 54 s, three stops, lower-third captions |
| `b-relentless` | **stops vs. one move** | 32 s, one unbroken zoom, no holds, all four terminals up front, 3 captions |
| `c-card` | **subtitle vs. title card** | full-frame typographic cards; the picture darkens behind the words |
| `d-silent` | **do words earn their place** | no captions at all until the closing title |
| `e-close` | **scale vs. legibility** | stops at 12 columns, not 44 — tiles stay readable as games |

## Findings so far (from the contact sheet, before the owner has watched)

1. **The beat-1 caption sits on top of the terminal output.** In `a` and `c` the
   "One command." card covers the very lines the shot exists to show — the command
   and what it printed. `d` (no caption there) is markedly easier to read. This is a
   bug in the brief's cut, not a property of captions: the fix is to move beat 1's
   caption off the terminal or delay it until after the flip. **Applies to whichever
   cut wins.**
2. **`e` ends on something you can point at; `a` ends on a texture.** At 44 columns a
   tile is ~45 px and the wall reads as woven fabric — impressive, abstract. At 13
   columns individual battles are still legible. These are different promises:
   *"this scales absurdly"* versus *"look, they are all real games"*. Genuinely a
   taste call, and the reason this variant exists.
3. **`c`'s cards read like a product launch and cost the picture.** They land harder
   than subtitles, but every card dims the thing being advertised. Probably right for
   one or two moments, wrong for all of them.
4. **`b` is the only one that never feels like it is waiting.** Its cost is that the
   filler desktop arrives at 9 s while the four heroes are still resolving, so the
   "exactly four windows" beat barely exists.

## Verdict

*(to fill in — which cut, plus any mix, e.g. "b's pacing with e's ending")*

- chosen:
- because:
- carry back into the real cut:
