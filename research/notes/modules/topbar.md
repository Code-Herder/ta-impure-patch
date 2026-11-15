# The top bar: the weather report and its repaint

The game's own top bar shows the wind, the tide and the game time to the right of the resource
bars, as TADR's weather report does, in skirmish and in multiplayer. Everything Impure draws on the
bar shares one repaint coordinator, which the share tick ([share](share.html)) uses too. The plan
and the owner's decisions are in [the weather report plan](../weather-report-plan.html).

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the plan.*

Addresses are VAs for our pristine build. `main` = `*(void**)0x511DE8`; player record `p` =
`main + 0x1B63 + p*0x14B`; side data `s` = `main + 0x37F3D + s*0x232`.

## The files

| file | thread | what |
|---|---|---|
| `tagpu_topbar.c/.h` | game | the repaint coordinator (`tagpu_topbar_frame`, `_fresh`, `_repaint`) and the weather report (`tagpu_topbar_draw`) |
| `tagpu_menu.c` | game | its post-GUI observer at `0x46A308` calls `tagpu_topbar_frame`, then the share tick, then `tagpu_topbar_draw` |
| `tagpu_wiki.c` (`tagpu_wiki_button_at`) | game | the wiki button's place as the last frame drew it, the right end the report keeps clear of |
| `mouse.c` (`mouse_client_map`) | game | a client point to the engine's own 1x point, HUD scale included, with no note of the pointer |

## What it shows

```
 Wind : +17 (6-30)
                       Game Time : 00:00:24
Tidal : +25
```

- **Wind** is the energy the mod's basic wind generator makes now, rounded: its `windgenerator`
  (unit def `+0x1D2`) times the engine's own wind ratio `main+0x37EDE`, the speed `+0x37EDA` over
  the limit `+0x37EC8` capped at 1.0 (`0x490D3B..0x490D7B`). The bracket is the map's range on the
  same scale: the least and greatest speeds `main+0x1425B` and `+0x1425F` over the limit, each
  capped at 1.0. A wind generator's yield is exactly this product
  ([weapons](../engine/weapons.html), the generator branch `0x401550..0x4016C3`).
- **Tidal** is the basic tidal generator's yield, `tidalgenerator` (`+0x1D6`) times TidalStrength
  `main+0x14267`, truncated. It does not change during a game.
- **Game Time** is GameTime `main+0x38A47` over 30, as hours, minutes and seconds.

**The basic generator** is the smallest `windgenerator` above 0 among the unit defs with no
`tidalgenerator`, and the smallest `tidalgenerator` above 0, found once a level from the defs below
the engine's own count, UNITINFOCount `main+0x1438F` (the defs at `main+0x1439B`, `0x249` apart).
Escalation's tidal generators carry `windgenerator` 10 beside `tidalgenerator` 1, which is why the
wind rule skips units with both. In OTA, ProTA, Total Mayhem, Twilight and Escalation the rule
picks ARMWIN's 30 and ARMTIDE's 1, the numbers TADR shows (its ARMWIN lookup, and TidalStrength as
it is). The game log names the two: `topbar: N unit defs, wind generator 30, tidal generator 1`.

**A mod with no such unit shows no such line.** TA Zero has neither and shows the clock alone, the
same result as TADR's dedicated Zero build, with no list of mods.

**Every player sees the same wind**, in multiplayer, because every peer draws it from one seed (the
engine patch at `0x490C5A`, `tagpu_patches.c`, *THE WIND*); in skirmish there is one wind. A
watcher and a replay show the current value as well: TADR shows them the range only, because its
replays did not reproduce the wind; Impure's recording carries the wind's state.

## Where, and how it looks

As TADR's `BlitWeatherReport` places it. The value column is 110 columns right of the longer of
the energy and metal bars (side `+0x52` and `+0x72`, right edges); the wind row is at the energy
income's text row (`+0xF6`, the y the block draws `+0xF2`'s text at, `0x4692D9`) and the tidal row
at the energy expense's (`+0x106`, `0x46936F`), falling back to the bars' tops for a side that
leaves those rects zeroed; the clock starts 117 columns right of the value column, halfway between
the rows. Each label ends at the value column, so "Wind : " and "Tidal : " right-align on it.

Words in GUI colour 7 and values in GUI colour 10 of the table `main+0xDCB` (TADR's
`RadarObjecColor`, the same table): `#ABABAB` and `#53DF4F` on the stock palette. The viewed
side's font (`+0x22E`), the one the bar's own numbers use.

**Held inside the bar.** The clock stops short of Impure's own emblems: its right end is the
wiki button's left edge (the leftmost emblem, `art/wiki/layout.json` `button`), taken from the
wiki's last drawn frame (`tagpu_wiki_button_at`) and mapped into the engine's columns with
`mouse_client_map`; the last value is kept while a frame draws the wiki window over the button,
and the screen width `main+0x37E1F` is the end before any frame has drawn one. The value column
then moves left only as far as the widest value, `+99 (99-99)`, needs. At 1024 wide this keeps
TADR's columns; at 1920 and above nothing moves. Rows are held inside the bar's 32 rows.

**HUD scale.** The report is drawn into the engine's 1x bar, which the UI layer magnifies, so it
grows with the bar and stays in place (measured at 1920×1080, 150 %).

## How it is drawn

From the post-GUI observer at `0x46A308`, inside DrawGameScreen after its GUI draw, with the
engine's own `SetFont 0x4C1420`, `SetTextColors 0x4C13A0` and `DrawTextCustomFont 0x4C14F0` on
the back buffer (`ctx` NULL), the font and colours saved and put back; widths come from
`0x4C1480(font, string)`. Those are the calls the UI layer captures, so the report reaches the
screen as the bar's own numbers do. Each line is drawn whole in the words' colour, then its value
again in the values' colour over the same glyphs.

**Only while the level is open and not closing** (`tagpu_packet_pub_level_open`,
`tagpu_reclaim_level_closing`), on a viewed record (`main+0x2A43`) below ten with its PlayerInfo
and a side below five with a font.

## The repaint

DrawGameScreen repaints its resource block, the whole bar's tiles included, only when the 33-byte
memo `main+0x37E3F` differs from the live values ([render](../engine/render.html), *The resource
block*). Text drawn on a bar that is not repainted stays, so a clock drawn again with a new second
would print over its old self.

- **Asking.** `tagpu_topbar_repaint` writes the memo's byte 0 with the viewed record's `+0x146` ^
  `0xFF`, a value the block never writes there, so the next block repaints. Asking again while one
  is pending writes nothing, and nothing is asked outside an open level.
- **Knowing.** `tagpu_topbar_frame`, first in the observer, reads the byte: once it no longer holds
  ours, the block of this DrawGameScreen (or an earlier one whose observer did not run) has
  repainted, and `tagpu_topbar_fresh` says so to every drawer this frame. The byte changes only by
  the block's copy-back or by another spoil of the same lever (`tagpu_gui_hook.c`'s, between
  flips), and every spoil is followed by a repaint before the next observer; the recorder leaves the
  memo alone (`replay-globals.tsv`, `resmemo`). One coordinator, because two drawers testing their
  own writes of the same byte would each read the other's.
- **Drawing.** The report on the bar is drawn again each frame, which changes no pixel. A different
  report waits for a repaint, the old one drawn meanwhile, so no frame shows the two over each
  other; a clock that ticks repaints the block once a second. A level starts with a repaint asked
  and nothing drawn until it is seen: the level load's reset `0x4679A0` (called at `0x491893`)
  does not guarantee one.

## Measured

- Skirmish on Two Continents, 1024×768 on the Vulkan lane: the three lines at TADR's columns, the
  wind moving +12, +16, +7, +5 over 20 s, the clock clean of old digits, clear of the income
  numbers and of the emblems; the raw fields read min 0, max 3000, limit 5000 (so 0-18) and
  TidalStrength 22.0. At 1920×1080 with HUD scale 150 %, the same in place on the magnified bar.
- Two-player network game on Two Continents: host and joiner showed the same wind at each of three
  paired shots (+16, +9, +9) and the same tide.

## Not covered

- Not run in a replay or by a watcher; the code path is the same, the viewed record's.
- An AI's generators yield 0.5× or 0.7× of this on difficulty 0 or 1 ([weapons](../engine/weapons.html));
  the report shows the unscaled yield, also when it views an AI's bar.
- Nothing on GDI.
