# The weather report on the top bar (the plan)

## Status

**Decided with the owner 2026-11-14..15 on an HTML mockup made from a capture of TADR itself; one
landing, built 2026-11-15 on `worktree-wind_tide`.** The top bar shows the wind, the tide and the
game time right of the resource bars, as TADR's weather report shows them, in skirmish and in
multiplayer. The module is [the top bar](modules/topbar.html).

The reference is TADR's own readout, captured from ProTA 4.8 as players install it (its cnc-ddraw
and TADR, no Impure) at 1024×768 on Canal Crossing: `Wind : +17 (6-30)` over `Tidal : +25`, and
`Game Time : 00:00:24` between them in the next panel, words grey and values green. Its code is
`cinomce.cpp:867` (`BlitWeatherReport`) and `HardCodeFunctions.cpp:526` (`GetWeatherReport`) at
TADR `dcff5dd`, read as a guide only: every fact the module rests on is from our disassembly.

## The decisions

| # | Question | Decision | Why |
|---|---|---|---|
| 1 | The look | **TADR's, unchanged**: three lines of text in the side's font on the bar, its columns and rows, its two GUI colours | The owner: "I want to have the same result visually as TADR, it looks good" |
| 2 | The draw method | **The engine's own text calls from the post-GUI observer** (`0x46A308`), not TADR's blit into the back buffer after the frame | It runs on the game thread inside DrawGameScreen, which owns what it reads, and reaches the screen through the UI layer as the bar's own numbers do; nothing on the render thread reads engine memory for it. The owner asked that it be safe by construction |
| 3 | Same wind for every player | **Already Impure's**: section B6 of the TADR port seeds every peer's wind from the session and the map (`tagpu_patches.c`, *THE WIND*, 2026-09-25); skirmish takes the same path. Nothing to build | Measured again here: host and joiner showed the same wind in a two-player game |
| 4 | Which generator the numbers describe | **The mod's basic generator, from its data**: the smallest `windgenerator` among units with no `tidalgenerator`, and the smallest `tidalgenerator`. A line is shown only when the mod has such a unit | TADR names ARMWIN; a name is a list. The rule gives TADR's numbers in every supported mod (ARMWIN 30, ARMTIDE 1; Escalation's tidal generators also carry `windgenerator` 10, hence "no `tidalgenerator`"), and TA Zero, which has neither, shows the clock alone as TADR's Zero build does |
| 5 | The file | **A new file, `tagpu_topbar.c`, with its own `thread-split.allow` line** (publisher, presentation), and one repaint coordinator for every drawer on the bar, the share tick moved onto it | The owner's call, 2026-11-15 |
| 6 | Watchers and replays | **The current wind, like a player** | TADR shows them the range only because its replays did not reproduce the wind; Impure's recording carries the wind's state |
| 7 | Impure's top-bar emblems | **The clock stops left of them**, the value column moving only as far as its widest value needs | At 1024 wide TADR's clock column reaches the wiki button; TADR has no emblems |

## The landing

One landing: the module, the coordinator, the share tick on it, the wiki button's place for the
right end, and a client mapping in `mouse.c` that does not touch the pointer's record. Verified on
the Vulkan lane at 1024×768 and at 1920×1080 with HUD scale 150 %, and in a two-player network game
([top bar](modules/topbar.html), *Measured*). Not covered: replays and watchers, GDI.
