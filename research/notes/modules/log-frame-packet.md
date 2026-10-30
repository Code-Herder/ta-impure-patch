# Log — the frame packet exchange, landings 1–4

What landings 1–4 of the frame packet exchange built, measured and found. The design is [frame-packet-exchange](../frame-packet-exchange.html).

*A frozen record: nothing new is written here. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.16 The frame packet exchange, landing 1 (`tagpu_packet.c`, `tagpu_packet_pub.c`, on by default, `tagpu_packet.off`) — 2026-09-12

The first step of the plan the cross-thread audit led to ([frame packet exchange](../frame-packet-exchange.html);
the audit: [cross-thread engine reads](../cross-thread-engine-reads.html)): every render-thread read of engine
memory becomes a read of a COPY the game thread published, handed over through one wait-free
four-slot exchange. Landing 1 builds the exchange, publishes a header-only packet, moves the one
render-thread read whose lifetime no note could establish — the marker text's font — into it as
bytes, and starts the build rule that keeps the rest from growing back. No world pass reads the
packet yet (landing 2: the view and the commands; 3: units; 4a–c: effects, fog, the GL UI's render
half).

| site | what we do there | thread |
|---|---|---|
| `DrawGameScreen 0x468CF0` | **observer** (`tagpu_detour_observe`, stolen `81 EC 14 02 00 00` — the six bytes `tagpu_menu.c` already observes; chained AFTER it, because the menu's `before` never hijacks the return and ours must). Every action gated on the return address **`0x4969D2`**, the in-play frame callback's call: `before` reserved for the commands (a no-op today), `after` = the publish, post-flip, **only when the cell holds no fresh packet** — the engine draws 330..4900 times a second against ~60 presents, so most draws cost one relaxed load (`skip=`) | game |
| `0x497C70` | **observer** on the loader thread's entry (stolen `55 8B EC 6A FF`, position-independent): logs the thread's id, the load-flag word and, at its return, whether the level's first in-play packet had already been published — the direct measurement of the loader thread and of the ORDER the in-play gate rests on ([engine map](../exe-reverse-engineering.html), "The in-play publish point") | loader |
| `0x483B53`, LoadMap's join (G20b) | **stub** (`tagpu_detour_land` over the seven stolen bytes `8B 54 24 40 C1 E2 0A`, position-independent, their flags dead; every branch of LoadMap's feature paths lands on this address): `pushad; push esp; call mapfeat_at_load; add esp,4; popad`, the stolen bytes, `jmp 0x483B5A`. Reads LoadMap's frame (the TNT's version, its cell records, the placement threshold), the FeatureDefs and the grid's height bytes, and builds the map's own features by LoadMap's rules on a heap grid of its own, handed to the game thread under a lock (§2.90; [engine map](../exe-reverse-engineering.html), "LoadMap's feature placement, and a saved game"). Writes nothing of the engine's. Installed only when the publisher is armed, and builds only under the Vulkan renderer; a site whose bytes differ is not installed and the edge stays black, which the `packet: the map's own features from LoadMap's join 0x483B53=0` line says | loader |
| hook 8, `0x469BD7` (markown's stub) | `tagpu_packet_pub_font_snapshot()`: whenever `[globals+0x204]` or its header signature changed, copy the font's header and its 95 printable glyphs into a game-side buffer, each as a one-glyph font object the blitter accepts; latch `[globals+0x208]`. Every packet carries the buffer (1612 B for the stock in-game font) | game |
| the teardown post hook (`tagpu_reclaim.c`, inside the `0x491B60` wrap) | `tagpu_packet_pub_level_end()`: a header-only packet with `in_game = 0` and the bumped generation, forced past the fresh gate, before the reader is released — without it the renderer would draw the dead level's last packet over the menus | game |
| `0x491B60`, the teardown, **only when `tagpu_reclaim` is not armed** (`tagpu_reclaim.off`, or its own install refused) | **observer** (stolen `A1 E8 1D 51 00`, the five bytes reclaim's wrap takes; hijacked return, so both of the function's exits — the `ret` and the tail-jump to `0x450DD0` — reach `after`): the same level-end packet, published by us. The out-of-game packet must not depend on another module being armed (landing review); with neither provider the publisher stays count-only, because no packet is better than a stale one. The launch line says which: `level-end packet by tagpu_reclaim's teardown post hook` or `by our own observer …`. **[SUPERSEDED 2026-09-12 by landing 3]** this line used to end "under `tagpu_reclaim.off` the level generation stays 0 for the session"; the publisher owns the counter now and advances it here too, which is what makes the template caches drop between levels with reclaim disarmed | game |
| `render_ogl.c`, around the overlay | `tagpu_packet_acquire()` once, before `tagpu_reclaim_pass_begin`, the pointer handed down through `TAGPU_FRAME.packet` / `TAGPU_FXVIEW.packet`; `tagpu_packet_frame_end()` after `pass_end`, unconditionally — the tail check and the heartbeat | render |
| `tagpu_text.c` | `tagpu_text_frame(packet)` copies the font area out of the packet once per font generation; the measure walks the glyph table, the raster hands each one-glyph object to `0x4CCF60` at the x the engine's own string loop would reach. **No `IsBadReadPtr`, no engine pointer on this path any more**; the GL UI's glyph cache (`tagpu_text_glyph`) keeps its probes until 4c | render |

**The primitive, in one paragraph.** Four slots — W (the producer fills it), READ and PREV (the
consumer holds them), and the one in the CELL, fresh or stale. The cell is one aligned dword,
`idx (2 bits) | FRESH`, reserved bits asserted zero. Each side exchanges a slot it holds into the
cell and takes what was there (`__atomic_exchange_n`, ACQ_REL — NOT mingw's `InterlockedExchange`,
whose contract is acquire-only), so the roles stay a permutation without a lock, provided the init
made them one: explicit at DLL attach, `W=0, cell=1 (stale), READ=2, PREV=3`, because zeroed
statics would put both threads on slot 0. `head_seq` stored before the fill, `tail_seq` after;
the consumer latches the head at acquire and compares the tail at frame end. Slots are 20 MB of
address space each (`PK_RESERVE`, derived from the unit tables at the design point — §2.86), reserved once, committed as the high-water mark rises on the producer's own
slot, never moved, never freed. A fill that does not fit is not published: the producer grows the
slot it still holds and fills it again (`pkx_publish`), which the consumer cannot see because the
slot is not exchanged until the tail is stored. The fill reports its whole size, the tables it
could not place included (`s_fillShort` in `tagpu_packet_pub.c`), so one refill fits unless an
input the render thread publishes changed in between. The loop keys on what `slot_commit`
committed, not on what it answered: past the reserve it commits all of it and still answers no,
and the record then carries the new capacity (the consumer refuses a `cap_bytes` that is not the
slot's) and is filled once more at it, which is the last fill. It is bounded because every pass
that fills again has grown the slot by at least its grain (64 KB, one page under `stress`) and the
reserve ends it. A frame is published cut only when the slot cannot grow to it, a failed commit
or a fill past the reserve, and that is `trunc`; past the reserve the fill runs at the whole
reserve, and what is cut is what the fill lays down last. The fog grids and the shade table go
down first, the world tables straight after them, and then the map's own features while the
mirror is taking them (§2.90), which the reserve holds at the design point (§2.86), so an
overflow reaches the build orders, the effects, the UI's render half, the font and, last, the
effects models' five tables (§2.89), which go together or not at all — a frame without its fog
grids would be drawn bare, one without its unit tables drops its whole unit hand-over, and one
without the map-feature table leaves the mirror off one more packet. The
publisher's counters count the fill that is published, once per publish: a fill done again takes
back what the one before it counted (`PK_FILL_COUNTS`), and a cache reuse in a refill is not
counted. Neither side ever waits. Every violation is counted, logged rate-limited, never
fatal: thread identity both sides (the render thread's restart across a display-mode change is
recorded, not refused — ownership is by role), the permutation after every exchange, `head ==
tail`, the structural bounds of every offset against the slot's committed size, a canary past the
capacity, one acquire per frame, the tail at frame end, the CRC under `check`.

**The chain rule, enforced.** `tagpu_detour_observe` chains a second observer onto a site by
turning the earlier stub's copy of the stolen bytes into a jump, so the `before`s run in install
order and each sees the engine's own stack — *unless* an earlier observer hijacks the return, in
which case a later one reads that stub's trampoline where the return address should be, and a gate
like ours on `0x4969D2` would silently never match. Since the landing review `tagpu_detour_observe`
refuses to chain onto a stub that has an `after` (returns 0, arms nothing): the hijacker is the
last observer on its site by construction, which is why `tagpu_packet_pub_init` runs after
`tagpu_menu_init` in `dllmain.c`.

**The build rule** (`tools/thread-split-check.sh`, a prerequisite of `ddraw.dll` in
`tagpu/ddraw/Makefile`, so `make -C tagpu/ddraw` and the CI job both fail on an offender; the
upstream `build.cmd`/vcxproj do not run it). A source not on `tagpu/ddraw/thread-split.allow` may
not name an engine virtual address (`0x4xxxxx` or `0x5[0-2]xxxx` — `.text` from `0x401000`,
`.rdata`/`.data` and its bss to the `.tls` at `0x52C000` — suffix-aware, because the `0x00511DE8u`
spelling 19 files use defeats a `\b`), include `inc/tagpu_engine.h`, probe with `IsBad*Ptr`, or
add an offset to `ta`/`main`/`main_p`/`cta`; comments and string literals are stripped first (a
log line naming an address reads nothing; the include is detected before the strip). It scans
`src/*.c`, `src/*.h`, `src/*/*.c` and `inc/*.h` minus the third-party GL and D3D headers. The
list started FULL — **42 files** after the merge with main (its `tagpu_hud.c` joined as a
publisher), each with its class (`publisher`, `session-reader`, `fenced`, `tooling`,
`pure-engine-code`, `engine-map`, `to-convert:N`), its scope (`sim` for the five files whose
simulation state is declared and checked by `tools/sim-state-check.sh`, `presentation` for the
rest: [completeness design](../tadr-port/demo-recorder-completeness.md)) and its argument — and only
shrinks. Verified:
a planted `*(int*)(*(char**)0x00511DE8u + 0x38A47)` in `tagpu_fps.c` (and again in `tagpu_cfg.c`)
fails `make`; removed, it passes. **What a text rule cannot catch, said plainly:** a main pointer
under a new name plus a macro offset, an address assembled from split macros, an address that
arrives at run time. It is a ratchet against the spellings in use, not a proof; a new spelling is
a review matter.

**Read it in `tagpu.log`.** `packet: ARMED 5 slots x 20 MB reserved, 127 KB committed each …` (the first 64 KB grain plus the
grain the canary's four bytes tip it into) and
`packet: publisher ARMED on DrawGameScreen 0x468CF0 … level-end packet by … loader-thread observer
at 0x497C70=1 …` at launch; then every 300 frames
`packet: pub= skip= overrun= foreign= acq= taken= gap= grow= commitfail= trunc= refill= viol=
pviol= crcbad= nopkt= | pub/s= taken/s= pubus p50= p99= | seq= tick= tps= speed= paused= in_game= gen=
flags= eye= vp= flips= font= fg= trunc= used= | draws= inplay= draws/s= inplay/s= foreign= deep=
fontcopies=<copies>/<refused> levelend=reclaim|own|none`.
`viol`, `pviol`, `crcbad`, `foreign`, `commitfail` and `trunc` must stay 0; `refill` counts the
fills done again after the slot grew under them (a few at a level's start, when the world tables
first outgrow the slots). **The segments' counts are per publish**, from the fill that was
published; `skip` is the fresh gate
working; `overrun`/`gap` are 0 in play **on a lane whose renderer takes packets**, and count
under `stress`, across a level end, **or from the roster keepalive** — the vulkan-only plan's
landing 10c-3 forces a fill when none has happened for 500 ms, so on `renderer=gdi`, where
nothing calls `tagpu_packet_acquire` at all, every one of those (≥ ~2/s — the force sets a
floor on the fill rate, not a ceiling) lands on a FRESH cell and counts as an overrun. **`gap` does NOT move with it on that lane** — `gap` is incremented only
inside `tagpu_packet_acquire`, so where nothing acquires, `acq`, `taken` and `gap` all stay 0 and
only `overrun` climbs. `gap` moves with `overrun` on the other case: a GL or Vulkan lane rendering
below ~2 fps, where the consumer does acquire and does miss sequences (the vulkan-only plan's
10c-3 row reads `overrun=2 gap=2`). Reading `overrun > 0, gap = 0` as "not the keepalive" is
therefore wrong, and this sentence said so until the landing review caught it. **Before that landing this line read "`overrun`/`gap` are 0
in play" full stop**, and on gdi it would now send you chasing a phantom. Note also that this
whole heartbeat is emitted from `tagpu_packet_frame_end`, whose only callers are
`render_ogl.c` and `render_vk.c` — so **on gdi the line is never printed and these counters
cannot be read at all**;
`tps` is `GameTime` per wall second and must read 3 × `speed`. A level change logs `packet: level
end -> gen N …`, then the loader thread's entry and exit and `packet: level gen N: first in-play
packet …`, in that order. Levers, read at attach: `tagpu_packet.off` (no slots, no publish, no
acquire; the observer stays in count-only mode so `draws/s` is still reported — **every string
through `tagpu_text_place` draws nothing under it**: the group digits, the `ShowRanges` labels and
the FPS readout, since the render thread has no font), `.check` (CRC-32 per packet),
`.stress` (publish on every draw with a garbage pre-fill, one-page slots that must grow, the
consumer sleeping 0..50 ms per take), `.poison` (the slot handed back is memset), `.show` (a
`PK<seq> T<tick> E<eye>` row under the FPS readout).

**The gates, measured 2026-09-12 on the reference setup, `200v200` at 1920×1080, `--maxfps 0`,
the play defaults:**

- *Protocol.* `check` + `stress` + `poison`: **10 205 taken frames, `viol=0 pviol=0 crcbad=0
  foreign=0`**, 639 181 publishes at 2 906/s (every in-play draw, the overrun path 628 975 times),
  `head_seq` monotone (626 778 sequence numbers skipped and counted as `gap`, never a step back),
  each slot grown once past its one-page start (`grow=4 trunc=1 commitfail=0`); publish p50 12 µs,
  p99 18 µs with the 16 KB garbage pre-fill and the CRC inside the timed region.
- *Cost.* `--maxfps 0`, the camera pinned, one 5-second heartbeat per sample. Armed, sim running: in-play
  `DrawGameScreen` calls/s **795, 826, 795, 847 (mean 816)**; one publish per presented frame
  (`pub/s` = `taken/s` = 307–314, the render thread's own rate uncapped), **publish p50 2 µs,
  p99 2 µs**; the in-play tick rate `tps` **59.79, 60.93, 60.40, 59.01 (mean 60.0)** at the live
  speed 20, i.e. 3 ×. Armed, the ARMOPT menu open (`paused=1`, the tick frozen at 2814 across a
  3-second peek): draws/s **741, 834, 846, 857, 902, 870 (mean 842)**. `tagpu_packet.off`
  (count-only observer, no publish, no acquire), sim running: **795, 770, 845, 783 (mean 798)**;
  menu open: **741, 790, 800 (mean 777)**; the tick frozen at 2815 across the same peek. So the
  armed arm is not slower in either state — it reads 2 % (running) and 8 % (menu) HIGHER — and the
  spread of the 5-second samples (±6 %) is wider than the 3 % band the gate named; the publisher's
  own cost is the measured 2 µs × ~310/s, 0.06 % of the game thread's second. The off arm's tick rate, by two `peek`s 20 s apart: **60.61/s** at the live speed 20 — the same 3 × as the armed arm's 60.0.
- *Levels.* two games in one process (`Tab → EXIT → MAINMENU → CHOICE1`, then `SINGLE → Skirmish →
  Mapping 1 → Start`). The log, in order: `reclaim: level teardown (gen 0 -> 1)`, `reclaim:
  teardown post: freed 279 block(s)`, **`packet: level end -> gen 1: in_game=0 published; 86137
  in-play draw(s) this level; load flags 0x0000`**, `native: level 0 -> 1`, the render thread
  restarted and the GL context changed, **`packet: loader thread 592 entered 0x497C70 (entry #2;
  load flags 0x0001; level gen 1)`**, **`… leaving 0x497C70 (load flags 0x0003 …; first in-play
  packet of this level not yet)`**, **`packet: level gen 1: first in-play packet at draw #86138
  (tick 0, load flags now 0x0003 …)`**, the context changed again. `viol=0` across the boundary;
  the first heartbeat of gen 1 carries `gen=1`. (The second game began PAUSED — `tick=0
  paused=1` — because the first was left in the ARMOPT menu; the engine's, not ours.)
- *The rule.* The planted offender above.
- *Text.* `one-unit` at 1920×1080, the commander selected, put in group 1 (the digit) and `+showranges`
  typed (nine def-range and three weapon circles, each labelled — `build distance`, `mincloak`,
  `weapon1 range`, `weapon3 range`, `sight` …), SHIFT held for the order block, the pointer parked,
  two `glshot`s 4 s apart per DLL. Noise floor (same DLL, 4 s apart): **0 differing pixels** outside
  the top-left readout corner, both DLLs. Previous DLL (`ddraw.dll` built from `1cb60b8`) against
  this one: **0 differing pixels** outside that corner — the digit's own 130×120 rect included —
  and the corner differs only by the readout's numbers and the new `PK` row. So the glyphs
  rasterised from the copy are the glyphs the engine's font pointer gave, pixel for pixel.
- *The two engine facts.* The loader thread's id is now measured at its entry (756 against the
  game thread 736 on the first run), and its exit is logged before the level's first in-play
  packet. Bits 0 and 1 of `main+0x38D75`: no instruction clears them; the packet's per-frame copy
  reads `0x0003` at the first in-play draw and `0x0000` a few seconds later, so a bulk store
  zeroes the word after the load, and the level-cycle run shows the word at **`0x0000` at the level end and `0x0001` at the next
  loader's entry** (bit 0 freshly set by the game thread, bit 1 clear), so a second game in one
  process starts from zero and the loader's bit 1 is a fresh fact each time — the ordering argument
  holds for every level, not only the first.

**Not closed here, by design.** No world pass reads the packet: the eye is still read from engine
memory by every pass and written by the zoom from the render thread (landing 2, with the
commands — the two must land together or zoom-to-cursor wobbles). The unit array, the fog grid,
the effects arrays and the GL UI's render half still read engine memory on the render thread and
say so on the allow-list (`to-convert:N`). The GL UI's string op still hands the render thread a
font pointer (4c). The `tick_start` stamp and `prev` are carried but unused until the lerp
rekeys onto them (3) — and until then the give-back is the plain one, so `prev` MAY carry the same
tick as the packet returned; the tick-aware give-back the plan's §5 describes lands with the lerp
that needs it. Under `tagpu_reclaim.off` the level generation never moves (0 for the session), so a
cache keyed on it would not drop between levels; the level-end packet still arrives (our own
observer), and nothing keys on the generation yet. *[The first sentence is landing 1's state;
§2.17 is what landing 2 closed, and landing 3's review closed the generation: it is the
publisher's own counter now and moves with or without reclaim.]*

### 2.17 The frame packet exchange, landing 2 — the view from the header, the commands the other way (`tagpu_packet.c`, `tagpu_packet_pub.c`, `tagpu_zoom.c`, `tagpu_vpwide.c`, `tagpu_input.c`, `tagpu_pal.c`) — 2026-09-12

The plan's row 2 ([frame packet exchange](../frame-packet-exchange.html) §11): the header becomes
the source of the one-eye record every pass reads, and every render-thread store into engine
memory becomes a **command** the game thread applies. After this landing no file on the render
thread writes a byte of engine memory, and the eye, the viewport, the palette and the gamma
reach it only through the packet.

| site | what we do there | thread |
|---|---|---|
| `DrawGameScreen 0x468CF0`, the observer's **`before`** (the in-play gate `0x4969D2`) | **the command apply**: take the latest record the render thread posted (a second instance of the packet's mailbox, latest wins, no waiting either side) and write every engine word it names, on the thread that owns them — after whichever of the frame callback's own camera writers ran this frame (the stepper and the scroll poll both precede the draw call, and both can be skipped: the stepper when paused, both under an in-game GUI screen) and before the draw's first read of the eye, which nothing inside `DrawGameScreen` stores ([engine map](../exe-reverse-engineering.html), "The commands, applied in `before`"). `tagpu_zoom_apply`: the level → the minimap rect's scale, `ScrollSpeed` at base/z; the terrain request → which range is in force (§2.3c); the replay's Follow ended when the scroll target at entry is not where its last write left it, or when the record's `follow_broken` (a wheel notch) or, for a Jump, `follow_seen` names its epoch, and the engine's follow released on an epoch's first write; a NEW record's anchor delta (`cum − applied`, consumed exactly once) → the follow released first when the gesture asked (`0x41C390`'s three stores), then eye and scroll target stepped together; the replay's Follow written to the eye and the target together, clamped into the range (the hold wins), and the epoch and its level published in the packet (`follow_epoch`, `follow_level`), which the render thread's follow lever pins the wheel to; the `tagpu_eye.txt` hold clamped into the camera range and written when it differs; the eye and target clamped into the range in force on every in-play draw (the walk home when the ground goes back to the engine); any eye moved → the minimap box through `0x466B70` and **bit 3 of `main+0x14281` cleared**, as `0x41CB6B` does. `tagpu_vpwide_apply`: the rect widened to the transform's range at the commanded level, or restored; W/H never written, a disagreement counted. Under `tagpu_packet.off` nothing is applied: the engine keeps its own range, rect and rate ([engine map](../exe-reverse-engineering.html), "The commands, applied in `before`") | game |
| the observer's **`after`** | the packet grew: `vp_addr` (the rect the engine can name, `+0x37E27..`), `pal` (1 KB, `main+0x143A7`), `gamma` (`[0x51FBD0]+0x614`, bounded), `cmd_ack_seq`/`cmd_ack_dx`/`cmd_ack_dy` (what the apply had done by this draw), `zoom_applied`, `cam_centre` (which camera range the apply chose for this draw) | game |
| the level teardown (the packet's level-end provider) | `tagpu_zoom_level_end` + `tagpu_vpwide_level_end` before the out-of-game packet: the engine's own range back in force and the eye and target walked into it, `ScrollSpeed` restored to the base, the true rect restored — the shell inherits nothing | game |
| `tagpu_overlay.c`, once per frame before any pass | `tagpu_zoom_read_lever(packet)`: the levers and the wheel's tween; the cursor anchor's step (zoom-in only) is pre-clamped against the same range computed from the packet's copy of the map size and the true viewport and added to a cumulative sum; the **predicted eye** = the packet's eye + the sum the packet has not acknowledged, clamped — what the native pass, the scaffold and every gather draw from (`tagpu_zoom_predicted_eye`). No packet, or an out-of-game one, is no world drawn | render |
| `tagpu_zoom_frame_end`, every exit path of the overlay frame | posts this frame's record: the level and whether a zoomed world is live (the game thread derives the rect and the scroll rate from the pair), the cumulative delta, `drop_follow` (a delta is still unacknowledged), the hold from `tagpu_input_cmd` | render |
| `tagpu_native.c`, `tagpu_scaffold.c`, `tagpu_gui_surf.c` (the layer draw), `tagpu_mark.c` (the build-cursor gate), `tagpu_pal.c` | read `vp`, the predicted eye, `vp_addr`, `pal`/`gamma` from the packet; `tagpu_vpwide_true_rect()` is game-thread only now; `TAGPU_FXVIEW` lost its `ta` field and the passes that still walk engine memory (units, features, effects — landings 3, 4a) read the main pointer in their own file, where the rule sees it | render |

**Prediction, and why there is no wobble.** A wheel notch is drawn on the frame it happens: the
render thread adds the step to its cumulative sum and draws from `eye + (cum − ack)`. The game
thread applies the same difference before its next draw and the packet that follows carries the
new eye with `cmd_ack_dx/dy = cum`, so the unacknowledged part goes to zero the moment the truth
arrives — prediction replaced, never added twice. The heartbeat's `unacked=(dx,dy)` reads (0,0)
at every steady state. The step is pre-clamped on the render side against the same range
(`range_pk`, the packet's `vp` and `map_pxw/h`, the same fields `zoom_eye_range` reads), so the
game thread's clamp fires only when the engine itself scrolled the eye to the edge in between,
and then the packet reconciles it like any other engine camera move. A frame whose predicted eye
the engine's grid does not span takes the WIDE fog grid, with the eye bounded into it (§2.3, "The
fog bound on the drawn eye"): the engine's grid spans the eye it was built at and its slack is
at most 32 px at 1×. So does a frame whose engine grid reaches more than one cell past the map
(§2.3c, "The fog").

**What went.** The render thread's stores of the eye, the target, the minimap rect, the viewport
rect and `ScrollSpeed`; the fog request/ack pair (`tagpu_zoom_fog_pending/seq/ack`) and
`terr_fogtick`'s OR of it; the follow-release request (`s_dropFollow`, `tagpu_zoom_follow_tick`);
the "residual kept whole while it waits" for the follow; the anchoring gate on terrown owning
the fog draw (the invalidation is the engine's own mechanism now, on the owning thread);
vpwide's W/H repair; `tagpu_zoom_eye_range`/`tagpu_zoom_eye_moved`; `tagpu_pal.c`'s two engine
reads; `tagpu_input.c`'s engine reads and writes; `TAGPU_FXVIEW.ta`. The allow-list lost
`tagpu_input.c` and `tagpu_pal.c` (40 files listed) and re-classes `tagpu_zoom.c` and
`tagpu_vpwide.c` as publishers.

**Read it in `tagpu.log`.** The `packet:` heartbeat gained `addr=(L,T,R,B) z=<applied> pal=1
gamma=` in the packet segment and a `cmd:` segment: `cmd: post= take= new= overrun= viol=
nocmd= seq= ack= unacked=(dx,dy) cum=(x,y) epoch=<seen>/<packet's> z= live= hold=` — `post` per
render frame, `take` per in-play draw, `new` the records actually new, `overrun` the posts nobody
took (all of the shell's, a handful in play), `viol` must stay 0, `unacked` (0,0) whenever no
gesture is in flight, `cum` the sum posted since the epoch began (it reads (0,0) again after a level
end) — and `vpapply= vpwh= applyus p50= p99=` at the end (`vpwh` must stay 0; `applyus` is the
whole apply, both modules, timed with the performance counter like the publish). The `.show` row reads `PK<seq> T<tick>
E<x>,<y> A<ack> D<dx>,<dy>`.

**The gates, measured 2026-09-12 on the reference setup, 1920×1080, `--maxfps 0`, the play
defaults, this DLL against the one built from landing 1's tip (`72772cf`):**

- *The zoom oracles* (`crowd-static` on Two Continents, viewport 128,32 1792×1016, `c =
  (1024,540)`, eye reset to (1728,702) between cases): control +6 at the centre **(0,0)**;
  upper-left (400,300) +6 **(−272,−105)**; lower-right (1700,900) +6 **(294,157)**; zoom out
  (400,300) −6 **(481,185)** — each exactly the predicted `(a−c)(1/z₀−1/z₁)`, each round trip
  back to **(1728,702)** with the level at exactly 1.000. `ScrollSpeed` 32 → 18 at 1.772, 57 at
  0.564, 32 back; the viewport rect (128,32,1919,1047) → (−769,−477,2815,1555) at 0.564 and back.
- *The hover oracle*: the pointer parked on ARMPW unit 1 (screen 600,188), `main+0x2CBA` reads
  `0xFFFF0001` at 1×, after +6 (1.772), after +9 (2.358) and after −9 back; the centre-anchored
  control puts unit 53 there.
- *The follow release*, measured twice. Paused (second level, the commander followed with Ctrl+C,
  tick 0): −4 notches at (900,600) moved the eye by exactly **(58,−28)** and zeroed
  `main+0x142F3`, with the release logged once; +4 at the centre moved nothing and left it set.
  And **with the sim running** (`one-unit`, tick advancing 60/s, after the review pointed out
  that a paused game never runs the stepper the claim is about): Ctrl+C eased the camera onto
  the commander at (705,1047); −4 at (900,600) moved it by exactly **(58,−28)** to (763,1019),
  zeroed the slot, and **three seconds and 750 ticks later the eye was still (763,1019)** —
  the stepper found no follow to ease it back to.
- *The apply's cost*: `applyus p50=2 p99=2` on every heartbeat of every run above — the whole
  apply, both modules, per in-play draw.
- *The epoch*: across the level cycle the render thread's sum read `cum=(58,−28)` at the end of
  the first level and `cum=(0,0) epoch=1/1` in the second, `unacked=(0,0)`, no camera jump at
  the new level's first draw.
- *Minimap rect A/B at zoom*: `main+0x142CB` at the same pinned eye, by the file lever — **1.0
  (27,6,43,15), 0.5 (19,2,51,20), 2.0 (31,8,39,13), back (27,6,43,15) on both DLLs**, the
  viewport rect and `ScrollSpeed` identical at each level too.
- *The repair counter*: `vpwh=0` over 305 779 in-play draws (two levels) and 1 571 648 under the
  stress lever.
- *Protocol*, re-run because the primitive changed: `check`+`stress`+`poison` on `200v200`,
  **21 427 taken frames, `viol=0 pviol=0 crcbad=0` on both exchanges**, `grow=4 trunc=1
  commitfail=0`, head_seq monotone, `vpwh=0` over its 1 571 648 in-play draws; publish p50 18 µs / p99 24 µs under stress, **2 µs / 2 µs**
  in play with the 1.6 KB header (1 KB of it the palette).
- *Levels*: two games in one process — `packet: level end -> gen 1`, the loader entering with
  the flag word at `0x0001` and leaving at `0x0003` before the level's first in-play packet,
  `viol=0` across, the second game's heartbeat at `gen=1 live=1`.
- *The camera hold*: `tacli eye` at (900,400) → eye and target (900,400); at (20000,20000) →
  clamped to **(8928,11656)**, the map minus the view; released, the eye stays.
- *Pictures*: `glshot` at 1× and at 0.564 anchored at (400,300) — the whole island at the
  lower level with the units held under the pointer, nothing torn.

**What the landing review changed (two reviewers, `high`, 2026-09-12; both could construct no
interleaving that breaks the mailbox or loses a delta).** Five real defects, all fixed on the branch
before landing: the camera hold outlived its file by up to 15 render frames (the hold is a level
the game thread re-applies every draw, and `s_holdValid` was cleared only by the 15-frame poll —
now every early exit of the file read drops it); one absurd number in `tagpu_eye.txt` made the
record's validator refuse the WHOLE record and with it every other command (the hold is clamped
at its source now); a notch in a level's last frames could be applied to the next level's camera
(the command apply's **epoch**: bumped at the level end with the applied sum reset, carried in
the packet, echoed in the record — an older epoch's record carries no delta, and the render
thread resets its sum when it sees the new one); the wide-fog gate missed the case where the
range walks the eye home ahead of the packet (the choice now tests the grid's span against the
drawn eye directly, the fog bound in §2.3); and the ordering claim was over-stated (above). Also from the
review: the count-only log lines said only that text goes blank; the UI layer's viewport rect
keeps the last in-game one on a frame with no in-game packet (an empty rect would have shown the
key fill raw for one frame at a level's tail); two stale comments in `tagpu_fogwide.c` and
`tagpu_mark.c`; the roadmap's row contradicted itself; two rows of the engine map had the
save-site strings swapped and the third copy site one instruction late. One residual named and
accepted: the message thread's ring test (`to_engine`) mixes the render thread's current level
with the addressable rect the game thread derived from a record up to one render frame old, so a
click exactly at the ring's edge during a wheel ease can be judged against a rect one frame
stale — before the landing both came from the same frame; the true-viewport gate (world or UI)
is unaffected.

**Not closed here.** The unit array, the fog grid, the effects arrays and the GL UI's render half
(the minimap surfaces, the cursor sprite, the string op's font pointer) still read engine memory
on the render thread — landings 3, 4a, 4b, 4c. A render thread that stops posting leaves the game
thread re-applying its last record's levels (the zoom, the hold), the same standing as the render
thread ceasing to write those words itself before this landing; there is no staleness rule, by
design (a count-based expiry is timing). `tagpu_packet.off` now means no world pass at all —
every pass reads the packet's view — and no commands applied: the engine's own camera range,
rect and scroll rate, and after ~90 frames its own terrain again once terrown's skip expires.

### 2.18 The frame packet exchange, landing 3 — the world in the packet (`tagpu_packet.c`, `tagpu_packet_pub.c`, `tagpu_native.c`, `tagpu_scaffold.c`, `tagpu_mark.c`, `tagpu_overlay.c`, `tagpu_lerp.c`, `tagpu_order.c`, `tagpu_feat.c`, `tagpu_render3do.c`, `tagpu_posebake.c`) — 2026-09-12

The plan's row 3 ([frame packet exchange](../frame-packet-exchange.html) §11): the units, their
pieces, the wrecks and the feature anchors cross in the packet, and **the audit's one open hazard
closes with them** — no file on the render thread walks the unit array, and none dereferences an
Object3do ([cross-thread engine reads](../cross-thread-engine-reads.html) §5 row 2).

| site | what we do there | thread |
|---|---|---|
| `DrawGameScreen 0x468CF0`, the observer's **`after`** | the packet grew four tables. `PK_UNIT` (104 B; 100 B before the owner's logo colour was added) one per LIVE unit, the walk bounded by the engine's own SLOT COUNT (`u16 main+0x14351`) rather than by the `begin`/`end` pair, and the table sized to that count so no player is ever cut; `PK_PIECE` (24 B) the COB move and turn triples and the flags, plus the TYPE's template node; `PK_WRECK` (44 B) the husks the anchors name, each carrying the ANCHOR TILE it was found on rather than its own position, because that is what the grid walk it replaced filtered on; `PK_ANCHOR` (16 B) the feature cells of the widest zoom rect with the six height bytes the engine's 2×2 projection average and the Classic++ ground gradient need. The header grew with them: the GUI colour table, the dispatched mouse point, the build cursor's corners, the two mode bytes, the option byte, the feature sweep, the two array COUNTS the fenced passes bound by, and the engine's 32×256 shade table — the per-map BASES are read live instead, because the teardown nulls them and a held copy would not know. Every offset and every bound: [engine map](../exe-reverse-engineering.html), "What the frame packet's publisher copies" | game |
| the same `after` | `tagpu_native_owns_unit()` is asked here, once per unit, with the def in hand — the answer crosses as one flag bit, so the unit pass, the marker pass and the composite wipe act on ONE answer instead of three threads' reads of the same bytes | game |
| the order-marker snapshot `0x469BFC` | every unit pointer a record carried becomes an ARRAY SLOT, and every def and weapon field the drawing needs becomes a number in the record: the nine labelled ranges, the three live weapon ranges and their AoE and attack runs, the build def's five footprint extents, the kamikaze set, the cursor sprite's GAF sequence | game |
| `tagpu_packet.c`'s acquire | **the rotation**: the frame instance holds THREE slots (READ, PREV and a SPARE) rather than two, so a packet of the same tick replaces READ and keeps PREV, and the pair the pose blend runs over always spans two distinct ticks. The choice is made AFTER the exchange out of records this thread owns — the two-slot alternative would have to peek at a slot the producer may be refilling | render |
| `tagpu_native.c` | the unit and wreck gathers, the whole pose path (`pose_accum_body`, `posed_pose`, `hires_pose`, `pose_dump`, the pose CRC), the ownership test, the ground height, the option bits, the sub-pixel table's key: all from the packet. What it still reads is per-LEVEL assets under `tagpu_reclaim`'s teardown fence — the model templates behind `MODEL_PTRS`, whose base is read LIVE every call (the teardown nulls it at `0x42DCD8`, and that null is the refusal) while the bound is the packet's `udef_count`, and the screen fog descriptor, which is landing 4b's | render |
| `tagpu_scaffold.c`, `tagpu_feat.c` | the anchors, the map and sweep dimensions, the units they stamp for; the FeatureDef and wreck RECORDS stay, under the fence. Their bases are read LIVE at every use (the teardown nulls both, and those nulls are the refusal) and their indices are bounded without a probe: a def row by `min(live NumFeatureDefs, the packet's)`, a wreck record by the engine's fixed 2048-record pool | render |
| `tagpu_mark.c`, `tagpu_overlay.c`, `tagpu_lerp.c`, `tagpu_hires.c` | off the allow-list entirely: they name no engine memory at all | render |
| the same `after`, on a level that plays a recording | **the replay's overlays**, copied by value from what the replay's frame visit built before the draw call `0x4969CD` out of the recording alone: `PK_OVERLAY` (16 B, at most 128: a recorded cursor or click, its player, its colour, a click's age and the world point), `ov_col[10]` (each recorded player's palette index) and `PK_UNIT.rsel` (bit p: recorded player p's selection holds the unit, drawn only while the slot holds a live unit of the entry's birth), in what was `pad_[3]` — `pad0` at 85 and `rsel` at 86, the record still 104 B, all asserted. The acquire bounds the count and every item's kind and player. `tagpu_mark.c` draws the cursors and the click rings after the build cursor with the fog off; `tagpu_native.c` draws a recorded selection as its selection rectangle in `ov_col` of the lowest player. Zero outside a playback, and on GDI nothing draws them ([perspective plan, 4c-3 as built](../tadr-port/demo-recorder-perspective-streams.html#4c-3-as-built)) | game → render |
| `tagpu_render3do.c` | the shade table and the build-state formulas take the packet's copies; the **write-back is deleted** | render |

**The lerp is rekeyed, and its arena is gone.** `tagpu_lerp.c` kept 3.4 MB of pose banks keyed on
`(Object3do, nparts, level generation)` and sampled `P_POS`/`P_TURN` on the render thread. The two
held packets are that history now: `prev` is an earlier tick and `read` a later one by
construction, units are matched by the STABLE ID (`unit+0xA8`) and verified on the model identity,
and the blend's clock is the two packets' own `tick_start` stamps — taken by the game thread the
instant it first saw each tick, within one engine draw of the boundary — instead of the learned,
smoothed period this module used to keep. The arena, the pointer hash, the free list, the ageing
sweep and the `LERP_BLOCK` 48-piece ceiling all go; a 256-piece model is now interpolable.

**What was deleted rather than converted.** The opt-in write-back (`tagpu_render3do()`,
`tagpu_writeback.on`, `writeback_paint`) — it walked an Object3do on the render thread and then
STORED into engine memory from there, which is the one shape the exchange exists to remove;
landing 2's "no render-thread store into engine memory remains" was true only because it was off
by default. With it went its FBO, its program and the four readback planes. Also gone: the model
probe in `tagpu_overlay.c`, and `tagpu_scaffold.c`'s whole-map feature census (`tagpu_features.trigger`
has answered that question properly since).

**Read it in `tagpu.log`.** The `packet:` heartbeat's packet segment gained `units= pieces=
wrecks= anchors=<n>(<cols>x<rows>) dup= pair= same=` — `dup` is the stable-id collision oracle over
one packet and **must read 0**, `pair` the frames that had a usable two-tick pair, `same` the
rotations that displaced READ because the tick had not moved. A `world:` segment at the very end
carries the publisher's own: `u= p= w= a=<anchors>/<cells scanned> scan=<scans>/<reuses> dup=
trunc=<units>/<pieces>/<wrecks>/<anchors> relbad= woob= shd=`. **`relbad` must stay 0** — it counts
draws on which `end != begin + (count−1)·0x118`, the relation the note records — and so must the
units, wrecks and anchors `trunc` counts, which are the tables' own caps. The pieces count is the
arena running out of the slot in a published fill, so it reads at most the packet's `trunc`. **`woob` is the wreck-record index the bound refused**,
added by the landing review below; it is a monitor and not the safety argument, which is the
engine's own 2048-record pool.

**What the landing review changed.** Two reviewers at `high` over the branch diff, one
working down the brief's risk list and one told to range freely. **Eight defects**, every one
verified against the pristine binary before anything moved — seven the reviewers' and one found
while verifying their first. All eight are fixed on the branch:

| # | what it was | why it mattered |
|---|---|---|
| 1 | the packet CACHED the three per-map array bases `main+0x1426F`, `+0x1420B` and `+0x14377` | **the worst of the eight.** The teardown frees each and then NULLS it (`0x4221F8`→`0x422214`, `0x42227D`→`0x42228B`, `0x42DCCB`→`0x42DCD8`), and that null is what refuses the walk on the next frame — the code this replaced read live and bailed on `ptr_ok(NULL)`. A copy taken at publish time is a pointer to freed memory that still tests non-NULL, so the fenced passes read straight through it for the length of a teardown, and with `tagpu_reclaim` unarmed for the whole cascade. The bases are read live at every use again; only the BOUNDS stay in the packet |
| 2 | the wreck-record index reached memory **with no bound**: the feature cell's raw `u16` times `0x30` off `main+0x1420B` | up to ~3 MB past the pool. A garbage "Object3do" that survives `ptr_ok` puts up to 256 rows of nonsense `node` pointers into the packet, **dereferenced afterwards on the render thread**. The engine's own read at `0x46A6C4` is unbounded too, but it only forms the address for a cell it is DRAWING, where this walk covers the zoom-floor rect plus a 32-cell margin. The bound is the engine's: `0x421F29` allocates `0x18000` bytes at stride `0x30`, so **2048 records** in stock; the raised limits make it 8192, and `WR_COUNT` is the same `TAGPU_LIM_WRECKS` (§2.6b) |
| 3 | `level_gen` was `tagpu_reclaim_level_gen()`, bumped only in reclaim's teardown post hook | with reclaim unarmed it never moved, so the PREV withheld across a level, the pose blend's refusal and **all four model-template caches** silently stopped invalidating. The publisher owns the counter now |
| 4 | an **in-range** permutation break was counted and carried on from | the cell held a slot the consumer already holds, so the producer wrote a slot it did not own — the partition the whole exchange rests on is gone, and continuing leaves a duplicate in the held set and one slot owned by nobody. It fail-stops now, like the out-of-range case |
| 5 | `tagpu_order.c` returned early on the explode RADIUS where the engine returns on the ExplodeAs weapon POINTER (`0x439125`) | dropped the second circle (`0x439196`) for a weapon whose `AoE>>1` is 0. Inert on stock content |
| 6 | a truncated anchor scan was cached as a complete one | every reuse publish of that tick under-reported `trunc` — 6 publishes in 7 |
| 7 | the wreck gather left the nanoframe group of the static `units[]` unwritten | a wreck on an index that held a unit under construction was staged and wire-drawn as one. **Present on main since G16**, not introduced here |
| 8 | *(found while verifying 1)* the FeatureDef index was bounded by the PACKET's `NumFeatureDefs` against a base read live, and two `IsBadReadPtr` probes stood in for the bound — one on the wreck record in `tagpu_feat.c`, whose index was as unbounded as the publisher's, and one on the FeatureDef record | across a level boundary those are two different maps. The engine grows that array one record at a time and writes the count **last** (`0x422543` reallocs, `0x422558` stores the base, `0x422DAC` increments the count), so the live count is always a conservative bound on the live base — the two passes take `min(live, packet)` now. `MODEL_PTRS` is the opposite order (`0x42D542` counts, `0x42D693` allocates, `0x42D6AA` stores), so a non-NULL base there already implies its count, and the packet's `udef_count` stands |

One more was a DOCUMENTATION finding and is acted on rather than fixed: every sentence in these
notes that called a per-level read safe "under `tagpu_reclaim`'s teardown fence" was true only
while reclaim is ARMED, and reclaim has five ways not to be. [Thread-safe
destruction](../thread-safe-destruction.html) §10b now carries the residual in full — what the live
bases and the new bounds do cover (a pass that starts after the teardown's null-out, and any index
straying outside the block), what they do not (a pass already inside the walk when the cascade
frees under it), and why there is no by-design fix in this landing's shape. It is an **open**
residual whose retirement is the plan's row 5.

Rejected: one PLAUSIBLE finding that the anchor rect's left margin is thin at the zoom floor. The
detector already exists — `outside=` on the `feat:` line, a flag set when the frame's rect asks for
cells outside the ones the packet carried — and it reads **0 on all 116 frames** of two runs at
`z=0.25`, the floor, with the eye thrown to nine map corners and back four times each, on a
forest fixture (`anchors=541` of 98 736 cells) and a wreck one (`anchors=730` of 142 136). Not
asserted this time: measured.

**The gates, measured 2026-09-12 on the reference setup, 1920×1080, `--maxfps 0`, the play
defaults, this DLL against the one built from landing 2's tip (`4b84098`):**

- *The protocol*, `200v200` under `check`+`stress`+`poison`, three runs of 300 s: **1 271 008
  publishes and 19 481 taken frames** (436 191/6 658, 376 318/6 177, 458 499/6 646),
  `viol=0 pviol=0 crcbad=0 foreign=0 commitfail=0` on both exchanges in all three,
  **`dup=0`** (the stable-id collision oracle) and **`relbad=0`** (the `end` pointer agreed with
  `begin + (count−1)·0x118` on every published draw), 0 `VIOLATION` lines in the log. **Re-run once
  on the binary that ships** after the review's fixes — which changed the exchange itself, adding a
  second fail-stop — for another **450 345 publishes and 7 377 taken frames** with the same five
  counters at 0, plus the new `woob=0`, `raced=0`, and `pubus p50 148 / p99 290 µs` under stress.
  `trunc=0/1/0/0` in every run is the growth path doing its job: the pieces slot truncates while it
  is still growing (`grow=22`) and never afterwards.
- *The pose CRC join* (`tagpu_posecrc.on`): **`raced=0`**. That is the gate this landing is
  measured by and its meaning changed with it — the race it counts is between the COB scripts on
  the game thread and the pose loop on the render thread, and the pose now comes out of a packet
  the game thread finished writing before it handed the slot over, so a non-zero count would mean
  the exchange itself is broken rather than a rate to accept.
- *The pose, held to the previous DLL directly*: on `selbox-slope` (three ARMSTUMPs on a hillside,
  real bank and pitch in the angle triple) the three units' `in`/`out` CRC pairs are
  **identical in both builds** — `42db193f`/`76a8ee95`, `65210a9c`/`22fc162a`,
  `40efe2fd`/`95f837bd` — and so is the whole `native:` counter line. The matrices, the shade
  rows and the visibility words the pass hands the GPU are byte-for-byte what they were.
- *Two level cycles in one process, three levels in all, run once with `tagpu_reclaim` ARMED and
  once with it OFF* — because the review's third finding was that the level generation only moved
  in the armed case, and the two providers of the level-end packet are the two halves of this gate.

  | | reclaim ARMED | `tagpu_reclaim.off` |
  |---|---|---|
  | provider on the heartbeat | `levelend=reclaim` | `levelend=own` |
  | the generations | `gen 1 (reclaim's 1)`, `gen 2 (reclaim's 2)` | `gen 1 (reclaim's 0)` |
  | the template caches | `native: level 0 -> 1` and `1 -> 2`, both dropping | `native: level 0 -> 1`, dropping |
  | `VIOLATION` lines | 0 | 0 |
  | `viol` / `dup` / `relbad` / `woob` / `trunc` | all 0 | all 0 |

  **The publisher's counter is not a divergent second counter**: with reclaim armed it tracks
  reclaim's exactly, and with reclaim absent it is the only one that moves. **The right-hand column's
  template-cache row is the fix.** Before it, with reclaim disarmed, no `native: level` line existed
  at all — a second level reusing a template address served the first level's answer for the life of
  the process.
  The loader thread entered three times and left before each level's first in-play packet, the
  command epoch tracked the generation (`epoch=2/2`), the new level's tables were sized to the new
  map, and the feature pass was unbothered by the new `min(live, packet)` def bound
  (`defs=442 junk=0 outside=0`).

- *Cost*. The publish is **p50 70 µs, p99 322 µs** at 354 units / 5 443 pieces / 35 wrecks /
  760 anchors, against landing 2's 2 µs for a header-only packet; the command apply is unchanged at
  p50/p99 **2 µs**. Two thirds of that first figure was the feature-grid scan (168 µs of 200 before
  it was cached per tick — 465 × 273 = 126 945 cells at the zoom floor), which now runs **735 times
  against 4 567 reuses** over one interval. Under `stress` the publish is p50 136 / p99 324 µs.

  **And it is not slower — measured twice, and the second time on the shipped binary.** On
  `200v200` (≈400 units) with the sim paused and the restore finished, the two DLLs run back to
  back for 60 s each:

  | pair | what | this DLL | `4b84098` |
  |---|---|---|---|
  | first | render frames / s | 207.5 | 178.5 |
  | first | in-play draws / s | 811 | 763 |
  | second, the shipped binary | render frames / s | **134.5** | 128.5 |
  | second, the shipped binary | in-play draws / s | **769** | 684 |
  | both | publish, game thread | p50 112 µs, p99 220 µs | p50 2 µs, p99 2 µs |

  **Read the direction, not the figure.** The method is identical in both pairs; what differs is
  how busy the GPU was, and that alone moves the level by 70 frames a second. So the only claim the
  measurement supports is the one both pairs agree on: the new build is **not slower**, and is
  modestly ahead on both. The publisher's 112 µs is real and is spent on the game thread; what buys
  it back is that the render thread no longer walks the unit array, the feature grid or an
  `Object3do` per unit per frame. This is not a speed landing and the number is not a target — it
  is recorded because a cost this shape could equally have gone the other way, and only a
  measurement tells the two apart.

- *Pixel A/B against landing 2's DLL* (`4b84098`), 1920×1080, `--maxfps 0`, the play defaults, the
  world viewport only, the animating cursor sprite's rect excluded as `uiwalk` excludes it:

  **Two different floors, and a row needs whichever is larger.** Within a launch, consecutive
  frames of a fixture with a burning wreck or an idling unit already differ — that is the
  within-launch floor. Across two launches of the SAME build they differ again and for a second
  reason: Classic++'s unit-atlas restore is lazy and time-sliced against a GPU budget, so the
  atlas it has finished is not the same one twice. On `selbox-slope` the within-launch floor is 0
  and the cross-launch floor is **26 pixels** — three clusters of single-channel ±1 on the three
  tanks, maximum channel delta 3, inside x 702..956, y 456..576. Measured twice, on two different
  builds, it comes back as the same 26 pixels in the same box. Every row below therefore carries
  both floors and the A/B, and the column that decides anything is the last one:

  | fixture | zoom | what it exercises | this DLL's floor | `4b84098`'s floor | A/B | **outside both floors** |
  |---|---|---|---|---|---|---|
  | `selbox-facings` | 1 | three tanks on the flat, the posed unit path | 0 | 77 | 0 | **0** |
  | `one-wreck` | 1 | the wreck table's own path | 0 | 0 | 0 | **0** |
  | `hires-vehicle-slope` | 1 | the glTF replacement pass | 0 | 0 | 0 | **0** |
  | `hires-wreck` | 1 | the replacement pass over a husk | 495 | 833 | 500 | **0** |
  | `selbox-facings` | 0.5 | the same three tanks, wide | 183 | 232 | 189 | **0** |
  | `feat-forest` | 0.5 | the anchor table over a forest | 178 | 165 | 206 | **0** |
  | `selbox-slope` | 1 | three tanks on a hillside, real bank and pitch in the angle triple | 0 | 0 | 26 | 26 — **under its own 191-pixel cross-launch floor** |
  | `cob-kbot` | 1 | a live COB script over a burning wreck | 780 | 414 | 738 | 18 |
  | `pose-inventory` | 1 | 69 units, all eight pose classes, every one idling | 7 140 | 9 194 | 10 126 | 731 |

  **Six of the nine are exactly 0**: every pixel that differs between the two builds is a pixel
  that differs between two frames of the SAME build. For the three that are not, the decisive
  control is this build launched TWICE and held to itself, which is the comparison the A/B actually
  is — and on all three **the build differs from itself by more than it differs from `4b84098`:**

  | fixture | this build vs ITSELF, two launches | this build vs `4b84098` |
  |---|---|---|
  | `selbox-slope` | **191** | 26 |
  | `cob-kbot` | **847** | 738 |
  | `pose-inventory` | **10 457** | 10 126 |

  There is no residue to explain after that, but each has a named cause anyway:

  - **`selbox-slope`.** Its 26 pixels sit in one box (x 702..956, y 456..576, maximum channel delta
    3) that reproduces on every measurement, on two different builds: three clusters of
    single-channel ±1 on three tanks of the same type, i.e. the same atlas texels three times.
    It is Classic++'s lazily restored unit atlas, whose restore is time-sliced against a GPU budget
    and does not land identically twice. With `classicpp` off the two builds agree to **1 pixel**.
    That atlas is also why this fixture's own within-launch floor is not a constant — 0 when the
    GPU was busy and the restore had stalled between two shots 3 s apart, 265 and 288 when it was
    free and the restore was still running.
  - **`cob-kbot`.** Its 18 pixels outside both within-launch floors lie at x 1509..1525, y 318..362,
    wholly inside the 45×65 box that holds the burning wreck's smoke plume — drawn by the particle
    pass, which is landing 4a's and untouched here.
  - **`pose-inventory`.** 69 units all running idle scripts. Its floor has been measured at 7 140,
    8 556, 8 957 and 9 056 within a single launch of one build. It is the pose COVERAGE fixture,
    not a parity one, and what actually holds this landing to the previous DLL there is the pose
    CRC join above: `raced=0`, with identical `in`/`out` pairs.

  **One fixture was DROPPED from the wide-zoom set, and the rosters are why.** An earlier sweep
  ran `one-wreck` at zoom 0.5 and it read 217 pixels outside both floors, 196 of them in the
  rightmost 32-pixel column of the viewport, where the picture is a unit clipped by the edge in one
  build and absent in the other. Reading `tacli roster --json` from both launches settles it:
  **they are not the same game.** One had 8 units and the other 9, the player's own commander
  started at world (3552, 1008) in one and (9283, 5088) in the other, and every AI's commander and
  mex was somewhere else again — **the skirmish assigns start positions per launch.** At zoom 1
  that camera sees no unit at all, which is why the same fixture is 0 there. A wide-zoom fixture is
  comparable only while no unit enters the widened view; `feat-forest` and `selbox-facings` are,
  and they are the wide-zoom evidence here.

  **Two harness traps cost a round of wrong numbers each, and both are worth writing down.**

  *A parity capture must wait for `restoreglsl: terr: done`, not for a fixed settle.* With several
  instances sharing the GPU the Classic++ restore does not finish in 30 s, and two mixed frames —
  part indexed art, part restored — differ by tens of thousands of pixels for a reason that has
  nothing to do with the build. One such pair read **71 558**. The ta-drive skill has said this
  since G14e; the first cut of this sweep did not do it.

  *And the cursor sprite must be masked from where the ENGINE has it, not from where you parked
  it.* `scenario load`'s `center_on` leaves the pointer on the anchor, and an injected `mouse:`
  does not always survive the engine's own polls: in one pair the new build's cursor sat at
  (60,1000) where it was put and the reference's at (950,529), the screen centre. That is a 21×23
  animated sprite in two different places — **195 pixels**, all of it the cursor, and 0 once both
  rects are masked. Read `[0x51FBD0]+0x1B6`/`+0x1BA` per instance rather than assuming.

  What is left after both is the fixture's own floor: **26 pixels** across two launches of this
  DLL, three clusters of single-channel ±1 on the three tanks — the same unit type, so the same
  atlas texels three times over. It is the Classic++ restored unit-atlas twin, whose lazy restore
  is time-sliced against a GPU budget: with `classicpp` off the two builds agree to **1 pixel**,
  the pose CRC join says the matrices, shade rows and visibility words are byte-identical, and the
  `native:` counter line is identical.

- *The rule*: the allow-list is **36 files**, down from 40, with `tagpu_mark.c`, `tagpu_overlay.c`,
  `tagpu_lerp.c` and `tagpu_hires.c` off it entirely and four more re-classed from `to-convert:3`
  to `fenced`; a planted `0x00511DE8` in `tagpu_mark.c` fails `make`.

### 2.19 The frame packet exchange, landing 4a — the effects and the particle layers (`tagpu_packet.c`, `tagpu_packet_pub.c`, `tagpu_fx.c`, `tagpu_sfx.c`, `tagpu_fxown.c`, `tagpu_gaf.c`) — 2026-09-12

The plan's row 4a ([frame packet exchange](../frame-packet-exchange.html) §11): the projectiles, the
explosions, the flying debris and the ten particle layers cross in the packet. **This is the one
client `tagpu_reclaim`'s per-LEVEL fence never covered** — the layer table is per game, but each
layer's `{begin,end}` pair and every object's sub-particle vector are `std::vector`s the game
thread GROWS mid-play, freeing the old array (`0x4732E0`), so the pair could be read skewed and a
consistent pair could name memory just freed ([cross-thread engine reads](../cross-thread-engine-reads.html)
§5). The probes both files carried made that rarer and nothing else.

| site | what we do there | thread |
|---|---|---|
| `DrawGameScreen 0x468CF0`, the observer's **`after`** | four more tables. `PK_PROJ` (44 B) one per live projectile: its position and tail, the weapon's model and its thrust flame as indices into the effects models' tables (posed on this thread with the rendertype's own rotation, §2.89), the weapon's colour NUMBERS already through `main+0xDCB`, the ground-shadow blob's world y, and the sprite or flare frame already indexed by this tick; `PK_EXPL` (24 B) its position, its body as an effects model and the two anim states' resolved frames; `PK_DEBRIS` (16 B) one per occupied particle slot, its position and its piece as an effects model; `PK_PART` (16 B) one per drawable sub-particle, **in layer order**, with the projection done (`x` = hi(world x), `zp` = hi(y) − hi(alt)/2) and its GAF frame resolved. The header gained the ALP/LHT capability bits, the ground-shadow frame, the 32×256 LHT ramp and the **whole 256-byte** GUI colour LUT | game |
| the same `after` | **the gather is taken once per SIM TICK**, keyed on `(level generation, tick)`. Measured on a live `200v200` at 1080p: **5 592 scans against 80 469 reuses, one gather in 15 publishes**, at 634 publishes a second against a 60 Hz sim. The LEVEL half of the key is what stops a hit across a boundary handing the native pass a model root the teardown has freed. **The argument is NOT the anchor scan's** — that grid really is constant within a tick because only the tick writes it, and these four are not: the engine's own explosion DRAW emits particles (`0x420B00` → `0x421550` at `0x420B18`, which calls the grey-smoke emitter `0x472810` and the fire emitter `0x472AB0`, both of which append to a layer). What licenses the cache is weaker: the tables are COPIES, so a later append cannot dangle one; positions are the TICK's, so nothing already present goes stale; and what it costs is the newest smoke or fire of a tick landing one publish late. [The sentence this row carried — "the engine's own draw passes read them and write nothing" — was disproved by landing 4a's review; §2.22 finding 10.] | game |
| `tagpu_fx.c` | the three effect tables and the engine's DRAWING RULES over them — which rendertype makes which primitive, where the shadow blob goes, how the lightning bolt jitters. It reads no engine memory at all and is **off the allow-list**; the models its records name are indices into the packet's model tables, which `tagpu_fxmodel.c` rasterises (§2.89) | render |
| `tagpu_sfx.c` | the particle table, walked by layer because the layer IS the draw depth (0..6 before the projectiles, 7..9 after the explosions). Also **off the allow-list** | render |
| `tagpu_gaf.c` | gained `tagpu_gaf_frame_geom` / `_subframe`, so a pass that only PLACES a sprite dereferences no engine byte of its own; the publisher calls its resolvers on the game thread | both |
| `tagpu_fxown.c` | the render thread's standing request for the tables (`want`), with the same 90-frame watchdog the two skip bytes stand on: an unarmed pass costs the publisher nothing | both |
| `tagpu_native.c` | **`g_wantBuilds`** [2026-09-14] — the BUILD GHOST's standing request for the packet's builds table, the same pattern: written by the render thread from the ghost's own 30-frame poll (`tagpu_native_set_want_builds`), read by the game thread in `fill_builds`, which otherwise walks the order arena and copies up to `TAGPU_PK_MAX_BUILDS * 16` B into the packet every frame for a pass that may not exist. Its 90-frame watchdog is `tagpu_native_flush_want`, driven from `tagpu_overlay.c`'s unconditional flush run — deliberately NOT from the setter, where the first version put it and where it could not see the one case it was for (a render thread that stops calling: a refused driver returns in front of the poll). The table is therefore CONDITIONAL; `tagpu_pk_builds()` returns NULL at `n_builds == 0` and every consumer loops to `n_builds`, so an absent table reads as an empty one | both |

**Three bounds became the engine's own, by disassembly** ([engine map](../exe-reverse-engineering.html),
"The effects: the four per-frame arrays"). `0x499A30` allocates the projectile array as `0x7D64`
bytes = exactly **300** slots of `0x6B`, and both append sites refuse past 300 (`0x49B6EE`,
`0x49B809`), so the render-thread pass's 8192-slot sanity cap is gone. The explosion add site
refuses past 300 (`0x420A42`). The debris slots are the 100 dwords at `0x511DF0..0x511F80`. **The
particle layer's bound is 401, not 400** — every emitter reads the size and `cmp eax,0x190 / jbe
append`, and past 400 destroys the FRONT object, shifts the vector down and appends anyway, so 401
is the steady state; a walk that stopped at 400 dropped the whole layer every time it filled, which
is what the new `layerbad` counter read **86** times on one `fx-mix` run before this was corrected.

**Two facts the landing established and the engine map now records.** The **"fx" GAF bank is a
SESSION asset**: `0x429870` loads it and resolves every named sequence into `main+0x147AB..`, and
its only caller is `0x49134D` inside `0x491200`, whose only caller is `0x49EA62` in WinMain
(`0x49E830`) — so the effect sequences are loaded once per process, not per level. And
**`main+0xDCB` is a 256-byte LUT**, not 64: `0x4AC7D0` writes exactly `0x100` bytes of it
(`0x4AC7FF..0x4AC88F`). The packet's `gui_col` grew to match, because a weapon's colour NUMBER
indexes that table and nothing bounds it below 256.

**What the gates measured (2026-09-12, 1024×768, `--maxfps 0`, the play defaults).**

- **A cross-build pixel A/B is NOT AVAILABLE on a combat fixture, and that is measured rather
  than assumed.** Two runs of ONE build on `fx-mix`, both paused at a fixed tick, differ by
  **39 478 px**: the sims reach different states (tick 649 against 655, `proj=3 expl=21` against
  `proj=0 expl=2`). Aligning the pause to a fixed number of ticks after the load does not fix it
  (784 against 810, `sub=130` against `sub=95`). What IS deterministic is one launch: the
  **within-launch floor is 0 px** on every state of every fixture measured.
- **So the effects gate is the ENGINE'S OWN DRAW of the same arrays, in the same launch, at the
  same paused tick.** Three frames: ours (`fx.on`+`sfx.on`), the engine's (both `passive` — it
  draws, we gather and count), and neither (both armed with every class muted, so we own the draw
  and emit nothing). `engine != neither` is where the engine put effects and `ours != neither` is
  where we put them, over one frozen world, with Classic++ off so both come off the same
  palette-indexed frames.

  | fixture | build | our px | engine px | both | IoU | ours-only | engine-only |
  |---|---|---|---|---|---|---|---|
  | `fx-mix` | **4a** | 13 436 | 13 280 | 13 207 | **0.978** | 229 | 73 |
  | `fx-mix` | landing 3 | 8 100 | 8 216 | 8 093 | 0.984 | 7 | 123 |
  | `sfx-strait` | **4a** | 777 | 777 | 777 | **1.000** | 0 | 0 |
  | `sfx-strait` | landing 3 | 1 048 | 1 181 | 1 048 | 0.887 | 0 | 133 |

  The two builds' scenes are not the same scene, so the counts are not comparable and the RATIO
  is. `fx-mix`'s 229 ours-only pixels are three clusters, and the largest was looked at: a smoke
  puff both builds draw in the same place, where our GL alpha blend covers a few pixels the
  engine's `AlphaCompsteBuf2OFFScreen` leaves cyan-fringed. That is the pass's own pre-existing
  character and scales with how much smoke is on screen.
- **The heartbeat's new `fx:` segment**: `proj= expl= deb= part=<this packet>/<high water>
  scan=<gathers>/<reuses> trunc= layerbad= subbad= lht= want=`. `trunc`, `layerbad` and `subbad`
  must all read 0. The particle high-water mark on the fixtures measured was **315** against the
  16 384-entry cap.

### 2.20 The frame packet exchange, landing 4b — the fog grids (`tagpu_packet.c`, `tagpu_packet_pub.c`, `tagpu_fogwide.c`, `tagpu_native.c`, `tagpu_terrown.c`, `tagpu_fx.c`) — 2026-09-12

The plan's row 4b: both fog lattices cross in the packet, and **`tagpu_fog_at`'s guard loses its
reason to exist**. The engine's own screen grid (`*(main+0x1421F)`) was read on the render thread —
the descriptor AND the buffer behind it — while this fork's terrain owner was calling the engine's
builder over both from the game thread. That is the read a hard fault off a base of −9 came out of
on 2026-09-03. **The root cause is still not found**; what changes is that the class is gone.

| site | what we do there | thread |
|---|---|---|
| `DrawGameScreen`'s `after` | the engine's grid, with its own relation checked (`cells == ((cols*rows + 7) & ~7)`, the allocator's round-up at `0x483C84`) and exactly `cols*rows` entries copied; `tagpu_fogwide`'s wide grid the same way; the grey band's 256-byte palette remap (`*(TAProgram+0xCC)`), latched like the shade and lighten tables | game |
| `DrawGameScreen`'s `before` and `after` | LosType bit 3 read either side of the draw: clear then set is a build inside it, and the eye after the draw is the one the builder read — the grid's build record, from which its world ORIGIN is taken, never from the packet's eye (§2.3e) | game |
| `tagpu_fogwide.c` | **one buffer, no lock, nothing retired.** The three buffers swapped under a critical section, the retire ring behind `tagpu_reclaim`'s quiescence fence, the wall-clock liveness test, `tagpu_fogwide_dimcap()` and the `ret=` / `held=` / `strand=` / `bare=` counters are all gone with the hand-over they existed for. A grow frees the old block on the spot | game |
| `tagpu_native.c` | both grids out of the packet; which one this frame uses is unchanged and still this thread's decision, because it is the thread that knows what it is about to draw | render |

**The bound is exact now, not generous.** The acquire checks `len == cols * rows * 2` against the
record's own extent, so the largest index a consumer can form is inside the bytes it was handed —
by construction, where before it was a per-dimension cap the two producers had to agree about (and
once did not: a screen between 4057 and 8153 px wide got a grid the producer built and the gate
refused, answering "nothing is hidden here" for the whole screen).

**The origin is right rather than right by coverage.** The render thread derived cell (0,0)'s world
point from its PREDICTED eye — the packet's plus a cursor-anchor step the game thread had not
applied — and covered the disagreement by taking the wide grid whenever anything was
unacknowledged. It still takes the wide grid there; the origin is now the eye the builder read.

**What the gates measured.** A static fixture is the one shape a cross-build fog A/B is available
on, and `selbox-slope` is one: `shootall: false`, no orders, a pinned camera, the sim paused as
soon as it is live, so the LOS state is a pure function of where the scenario put the units.
Loaded `--mapping 0 --los 1` so there IS fog, at four stops — zoom 1.0 and 0.5, at the camera's own
position and at the map's (0,0) corner, zoom driven through `tagpu_zoom.txt` so the camera never
moves between the first two. **Every stop: within-launch floor 0 px, and 0 px between landing 3's
DLL and this one.** `fogwide check: differ=0` on 720 of 720 cells on both builds; `bare=0`.
The heartbeat's new `fog:` segment is `<cols>x<rows> wide=<cols>x<rows>/<publishes> refused=`,
with `norec=` and `recodd=` since the engine grid's build record (§2.3e); its `shade=` went with
the fog-shade table (G20d).

**And re-measured on the binary that SHIPS**, after the review's fix put a draw stamp in front of
both fog answers — because that fix could have refused the wide grid outright and the only thing
that would have said so is a picture. Same four stops, same 0 px within a launch and **0 px against
landing 3's DLL**, `bare=0`, `fogwide check: differ=0`.

### 2.21 The frame packet exchange, landing 4c — the GL UI's render half (`tagpu_packet.c`, `tagpu_packet_pub.c`, `tagpu_gui_surf.c`, `tagpu_gui_hook.c`, `tagpu_gui_leaves.h`, `tagpu_gui_int.h`, `tagpu_text.c`, `tagpu_overlay.c`) — 2026-09-12

The plan's row 4c and its §9. The GL UI layer is unchanged except in where its render half gets
its inputs; the op QUEUE is untouched and stays a queue. Full detail:
[GUI renderer](../gui-renderer.html) §23. In one table:

| what it was, on the render thread, every present | what it is |
|---|---|
| the cursor's position and sprite record through `[0x51FBD0]+0x1B6`/`+0x1BA`/`+0x1B2` | the record alone, as the header's `cur_rec`: a KEY into the session cursor table, and only `tagpu_gaf.c` dereferences it. The position has no reader and does not cross |
| the minimap's box, its view rect and its colour byte | header fields |
| its three 8bpp surfaces, walked row by row to interleave into RGB | one area the PUBLISHER interleaves, gated on the sharp minimap asking for it (at k = 1 it is deliberately the engine's own) |
| the level's picture, from a buffer the LOADER thread filled | the level's FIRST in-play packet, acknowledged by the consumer; **the loader-thread observer is deleted** |
| `PK_STRING.frame`, the engine's FONT OBJECT, read per glyph behind `IsBadReadPtr` | a font ID and each glyph's width and packed ROWS, copied on first sight of a (font, code) pair on the game thread |

**The loader-thread observer went because a fact in this fork's own notes was wrong.** They said
the minimap picture `main+0x1426B` was alive only inside `BuildMinimapSurface 0x466780`. It is
alive for the whole level: LoadMap stores it at `0x483900`, `0x466780` reads it at `0x46684F`
without nulling it, and the only free is `0x483DFE` inside `0x483DD0`, whose only caller is
`0x491BB3` — the teardown cascade. So the publisher decodes it itself on the level's first in-play
draw, and **nothing of ours runs on the loader thread any more**, which is the second of the two
things the row exists to close.

**Not closed, and named rather than found later: on a SHELL frame the cursor is the engine's own
again.** Its position and sprite were read live out of the graphics globals on the render thread —
in play and on the menus alike — and the publisher only publishes from the in-play gate, so a shell
frame holds the out-of-game packet and reports no rect. At k = 1 the same art; at k > 1 the
engine's sprite blown up instead of ours at the device's resolution.
[GUI renderer](../gui-renderer.html) §23 *Not closed here* has it.

**`tagpu_gui_surf.c` is off the allow-list and `tagpu_text.c` carries no probe.** The blitter
`0x4CCF60` is still `pure-engine-code` and is now the only thing on that list from either text
path: both hand it a one-glyph font object of ours, so TA's own glyphs are still TA's own blit.
The list is **33 files**, from 36 before landing 4.

### 2.21c Landing 4c's own gates

The static fixture of §2.20 again — `selbox-slope`, `--mapping 0 --los 1`, paused as soon as it is
live — with `gui.on=mmbase` forcing the sharp minimap on at k = 1, which is the only way the k = 1
comparison can be taken at all (§19). **The whole UI frame: 0 px within a launch on both builds,
and 0 px between landing 3's DLL and this one** — the minimap drawn from packet-carried surfaces
against the same minimap drawn from live engine reads, with `fog=13333/13356` of its texels masked
on both, so the fog mask was actually exercised rather than inert.

The counters, both builds: `curs=1,10x20,dev=1` — **our** cursor, at the 10×20 device footprint
that says ours is drawn and the engine's erased; `str=…,miss=0,reseed=0` — **no glyph the cache
refused that the engine would have drawn**, over ~14 700 presented frames of text; `lost=0
strict=0 overflows=0`. The packet's own segment reads `mm=106x126/14726 refused=0 pic=252x252/2`:
the picture went in **two** packets before the consumer's acknowledgement stopped it, which is the
acknowledgement working, and `noeng=1` against the reference's 0 is the single warm-up frame the
standing request costs.

**The 120-stop `strict` UI walk, and the one column that moved.** `tools/uiwalk.py --inst <i>
--res 1024x768 --layer --cycles 2`, run on landing 4's build and then, identically, on landing 3's
— with `classicpp.on=off`, which is not optional: the walk diffs our GL frame against the engine's
**indexed** surface, and the restorer makes the two halves come off different art. **Seventy stops compared
column by column, and every column that is a measurement reads the same on both builds**:

- `hit misses` and `drift px` **identical at every stop**, the one standing `MISS=1 [FPS]
  drift=23px` on `VISUALS.GUI` included — it reproduces on landing 3's DLL at all three of its
  cycles, so it is `uiwalk`'s hit-test inverse on a row `tagpu_menu.c` emits (*The front-end screen
  carries fifteen rows in two columns*, above) and not this landing's.
- `differing px outside the viewport` **0 at every stop** but the four `MAINMENU.GUI` ones, which
  the [ta-drive](../../../.claude/skills/ta-drive/SKILL.md) skill already records as that screen's own
  sparkle animation between the two shots: 181–184 here against 179–188 on the reference.
- `inside the viewport, engine non-key px` **0 / N at every stop** but `space-popup`, and that
  stop is unstable on both builds rather than different between them: 41 of 37 294 here against 22
  there, in the same 6x8 box at the bottom of the viewport, with the engine's OWN between-shot
  self-difference reading 15 here and 93 there. The popup is mid-animation in both, and the
  engine's two captures do not agree with each other either — it is a stop to re-take, not a
  number to compare.
- The two 8-column census tables are **identical**: `censuses`, `changed px`, `unexplained px` and
  `ops in the window` match at all seventy stops.

**The one column that moved is `strict holes`: 4 on landing 4's DLL at two in-game stops (`F4@0.5`
and `move`), 0 on landing 3's** — and the cause is worth the space, because it is a property of
reading the cursor from the packet and not a mistake in the conversion.

The engine's cursors PULSE. The `curs=` counter reports the move cursor at 27×27, 29×29, 31×31,
33×33 and 35×35 on **both** builds — one pixel per side per animation step. The rect the layer
exempts from the fallback and from `strict` used to be read live on the render thread; it is the
packet's now, taken inside `DrawGameScreen`, and the engine blits its cursor onto the primary
*after* that. So the surface the layer composites against can already hold the NEXT step, and where
the next step is larger its outermost ring is neither drawn by our sharp layer nor exempt. On this
art — a four-way arrow whose outermost ring is four isolated tips — that ring is exactly **four
pixels**, at (496,384), (512,368), (512,400) and (528,384) around a pointer parked at the screen
centre, with the engine's two surface captures **byte-identical between the builds** (0 px) at that
stop. It is one animation step, at two of forty-five in-game stops.

**What a player sees is not magenta.** `strict` is the harness's mode; with the fallback on, those
four pixels show the engine's own cursor art, so the frame carries the union of two adjacent steps
of the same sprite — a cursor that looks one step thicker for one frame. **The fix is a bound, not
a margin**: the exempt rect has to cover the sprite the engine will blit *next*, which means
publishing the cursor's animation extent out of the cursor table — per-process and immutable, so a
constant once read — and nobody has read that table's shape yet. Padding the rect by a guess is the
timing argument `CLAUDE.md` refuses, so it is named in
[GUI renderer](../gui-renderer.html) §23 *Not closed here* and left for the landing that publishes it.

### 2.21b The protocol and the cost, measured across all three rows

**The protocol**, `200v200` at 1920×1080 under `check`+`stress`+`poison`, on landing 4's DLL and on
landing 3's, back to back: **10 487** taken frames against 10 564, and on both exchanges of both
builds `viol=0 pviol=0 crcbad=0 foreign=0 commitfail=0`, with `dup=0 relbad=0 woob=0` in the world
segment and `trunc=0/0/0/0 layerbad=0 subbad=0` in the effects one. `grow` and `trunc` past the
first fill of each slot are the stress lever's growth path doing its job and are 45/19 against
21/11 — landing 4's record is bigger, so it grows more.

**The packet is bigger, and most of it is one table.** `used=` on `200v200` at 1080p went from
**65 KB** to **168 KB**. Turning the two new gathers off in the same launch says where it goes: the
three effect tables and the particle one are **8 KB** and the wide fog grid is **72 KB** (245×148
cells at 1080p, two bytes each). The engine's own fog grid is 4 KB, the LHT ramp 8 KB, the fog
shade 256 B.

**The wide grid is copied into every packet on purpose, and the alternative was considered and
rejected.** It could be sent only while the render thread is drawing at zoom < 1 — the publisher
knows the commanded zoom, because the command record carries it — but the render thread decides
mid-frame to draw the first zoomed-out frame of a gesture and posts that command at the END of its
frame, so the game thread learns one render frame late. That is exactly the bare frame G13s
removed: one frame of the outer ring drawn over the engine's 1× grid, at the start of every
zoom-out. The copy is what buys "never a bare frame", and it is 72 KB of memcpy on the game thread
per publish.

**THE COST, measured inside ONE launch, because a cross-build one is not available.** Two runs of
`200v200` reach different scenes — the same reason the effects pixel A/B is unavailable — so the
figure that means anything is the same launch with the new gathers turned off and on again. On a
settled 55-unit scene at 1920×1080, `--maxfps 0`, no stress levers:

| phase | publish p50 | p99 | packet | in-play draws/s |
|---|---|---|---|---|
| everything armed | **46 µs** | 234 | 153 712 B | 3389 |
| the four effect tables off | 44 µs | 214 | 147 280 B | 2421 |
| ...and the wide fog grid off | **38 µs** | 224 | 74 760 B | 2374 |
| everything armed again | 44 µs | 230 | 152 912 B | 3540 |

So **the effect tables cost 2 µs of publish and 6.4 KB, and the wide fog grid 6 µs and 72.5 KB** —
8 µs of a 46 µs publish, and 79 KB of a 154 KB packet. The publish's own cost is still dominated by
landing 3's world gather: at ~340 units the same run read p50 112 µs and 308 KB, and at 55 units
46 µs.

**Landing 3's DLL, on a 63-unit scene of the same run's shape, reads p50 30 µs and a 66 392-byte
packet against landing 4's 46 µs and 153 712** — two different battles' leftovers, so it is a
supporting figure and not the measurement; the within-launch phases above are. Turning both new
gathers off inside landing 4's own launch lands at 38 µs and 74 760 B, which is the same place from
the other side.

**The whole build is not slower.** In-play `draws/s` reads 3389 and 3540 on landing 4 against 3347
and 3293 on landing 3, at 55 and 63 units. (The `draws/s` figures for the middle two phases — 2374
and 2421 on landing 4, 1794 and 1850 on landing 3 — are *lower* on **both** builds, because
`fx.on=off` hands the engine back its own projectile, explosion and particle draws and it pays more
for them than our pass does. That is a fact about the effects pass and not about this landing; it
reproduces on the DLL that predates it.) Every one of these is a ratio and nothing else
([ta-drive](../../../.claude/skills/ta-drive/SKILL.md), "a frame-rate figure is only ever a RATIO").

### 2.22 What landing 4's review changed

**FIVE reviewers at `high`, read-only, launched as `Agent`s and never `/code-review`** (a fork
runs on the session model). Four read the landing — one per plan row with a numbered risk list, and
one told to range freely and to check the notes' claims against the pristine binary — and a fifth
read the fix diff afterwards. **Fourteen findings between them**, every one verified in the code or
the disassembly before anything moved and every one acted on — plus a fifteenth, rejected below
with its reason, and three further documentation corrections. Twelve of the fourteen were
correctness and two were documentation, and it is the documentation pair worth noticing, because
one of them is the sentence that licensed a cache.

| # | what it was | why it mattered |
|---|---|---|
| 1 | **both fog answers froze silently** when `terr_fogtick` stopped running | our fog observer runs only while `g_terrown_skip` is set, and the render thread drops that on six documented paths (the `terr.on` lever, `passive`, `over`, a `key=` change, a bail-out, the 90-frame watchdog). `tagpu_fogwide`'s "valid" flag is only ever cleared from inside the tick that has stopped, and the eye latch was never cleared at all — so the packet would have carried the **last grid ever built, for ever**, against a camera and an LOS state that keep moving, with nothing counting it. The wall-clock liveness test landing 4b deleted was never about the hand-over; it was about the producer stopping. The observer stamps the publisher's in-play draw counter now and the publisher accepts either answer only when the stamp is this draw's |
| 2 | **the eye latch outlived its level** | a fresh level can go many draws before the engine's fog builder next fires, so the old level's origin would stand until then. It carries the publisher's level generation too |
| 3 | **the engine's effects draw was suppressed before our tables existed** | the request travels render → game and the fill comes back, so the first frames after `tagpu_fx.on` appears carried empty tables while the pass had already claimed the draw: a handful of frames with no fire, explosions or debris at all. The packet carries `fx_want` and the skip follows it |
| 4 | **a glyph marked sent could be lost for ever**, two ways | the consumer's atlas resets on a shelf overflow and on a ninth font, and the producer's `sent[]` survived it; and an op the consumer DROPS (a surface with no twin, the frames after a reseed) took its glyph records with it. Either way every later string in that font draws with those characters **missing and the rest closed up**. The producer watches the atlas generation, and the drain loop installs the block for every string op before the twin lookup |
| 5 | **the minimap picture rode in exactly one packet** | the mailbox is latest-wins and a dropped packet is a counted statistic — and the likeliest one to be dropped is a level's first, when the render thread is busiest. The render half acknowledges it and the publisher sends until it does |
| 6 | **codes 0x7F..0xFF stopped reaching the glyph cache** | `0x4CCF60` bounds a character below `first` and NOT above, which is why the cache runs to 0xFF; the producer's range was 0x20..0x7E, so a high code drew nothing AND did not advance x |
| 7 | `kind` and `layer` were the only packet indices a consumer formed without a bound at acquire | both are checked in `frame_valid` |
| 8 | **the effects atlas keys on a GAF frame's ADDRESS** and was never invalidated at a level boundary | pre-existing, and finding 11 is what made it matter: a second level's allocator can hand a new frame an old one's address. It drops on the packet's level generation |
| 9 | `tagpu_packet_pub_level_end`'s foreign-thread return skipped the picture reset | the picture's state is keyed on the level generation now and needs no reset at all |
| 10 | **the per-tick cache's stated argument was disproved by the binary** | it said the engine's draw passes read the effect arrays and write nothing. **They do not**: `0x420B00`'s debris loop calls `0x421550` (`0x420B18`), which calls the grey-smoke emitter `0x472810` and the fire emitter `0x472AB0`, and both append to a particle layer. So the layers are not constant within a tick. The cache stands on three weaker things instead, and the comment now says all three: the tables are copies so a later append cannot dangle one; positions are the tick's so nothing already present goes stale; and what it costs is the newest smoke of a tick landing one publish late |
| 11 | **an explosion's two anim states are PER-LEVEL**, not from the session `"fx"` bank | the add site takes the sequence from `main+0x1AB8F[idx]` (`0x420AA2`), a table `0x420620` builds from the level load and `0x420960` frees and nulls from the teardown. The notes said this was "not established"; it is now, and unfavourably — those two frames stand on `tagpu_reclaim`'s fence exactly as the model templates do |

**A FIFTH REVIEWER read the fix diff**, because fixes of that size introduce new mechanisms and a
fix is exactly where a landing stops paying attention. Three of the eleven had added a cross-thread
word. It found **three more, all confirmed**, and they are the reason that pass was worth running:

| # | what it was | why it mattered |
|---|---|---|
| 12 | **the glyph-feed fix was bypassed by the skip-to-reset path** | `drain()` jumps the whole switch for every op queued before a GL context change — its own comment measures 705 of them — so installing the block *inside* the switch still let those ops take their first-sight glyph records with them while the producer had already marked the pairs sent. The block goes in before the skip gate, and the producer's reseed handling clears `sent[]` alongside the sprite and pixel tables it already re-arms. Finding 4's own commit message said "every string op"; it was not |
| 13 | **the fog stamp proved the observer ran, not that the latch was fresh** | after ownership is dropped and returns, the engine rebuilt the grid at a live eye where we could not see it, and on the first tick back `LosType` bit 3 is already set — no rebuild, the stamp matches, and the pre-gap eye goes into the packet against a live grid. Handing the fog site back clears the latch |
| 14 | **the minimap ack could be raised for a picture nobody has** | with `gui.on='mmbase nominimap'` the render half acked without a copy; dropping `nominimap` mid-level then left the level with no picture and nothing to recover it. The ack is withdrawn whenever the copy is absent, which makes every reason it can be absent self-healing |

It also caught the cursor rect's **fourth** outcome, which finding 11's fix had collapsed: a
readable sprite record of zero extent gives the position with a zero size, not "no rect". Making
that exact meant gating on `in_game`, which surfaced the one thing this landing did not close —
**on a shell frame the cursor is the engine's own again**, named above. **Closed 2026-09-13 by
landing 6** (`tagpu_packet_pub.c`, the shell cursor channel, observed on the flip's cursor draw
`0x4C67C0`): the gate is `in_game || cursor_live` now, and the shell publishes its own
header-only packet. (Since 2026-09-23 `cursor_live` is gone and a non-zero `cur_rec` is the
whole test.) Measured on the main menu: `curs=1,10x20,…`, and the cursor's zone is
**0 px different** between the layer on and the layer off.

**One finding was rejected, and here is why.** The producer publishes glyphs for `0x20..0xFF` and
the reviewer observed that `0x4CCF60` refuses only `0x00` and `0x0A`, so `0x01..0x1F` could in
principle be glyphs too. They could — but `tagpu_text.c`'s cache has always started at `0x20`
(`CH_LO`), so the producer's floor matches the consumer the landing replaced exactly and **nothing
regressed**. Lowering it means widening the consumer's atlas as well, which is a change to a landed
gate for a case no engine UI string contains.

Three more documentation corrections came with them: the particle emitter list undercounted
(**twenty** sites cap a layer, not thirteen — all twenty are listed now, because a partial list
invites the same mistake twice); `f[3]` is the font's FIRST CODE (`0x4CCF77` / `0x4CCFAA`), not the
high byte of the y-offset word, so the `f[3] != 0` test is a refusal of an unusual font rather than
a statement about the format; and `TAGPU_PK_MM_DIMCAP` is a sanity ceiling of ours, the engine
bounding only the BOX it fits the picture into. **The publisher's own comment and its launch log
still said the level generation was reclaim's and "0 for the session" with reclaim off** — the
pre-landing-3 design, changed by landing 3's own review, with the text left behind. A reviewer read
it and reported a defect that is not in the code, which is the cost of a stale comment stated as a
measurement.
