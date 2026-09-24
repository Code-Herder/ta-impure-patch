# Cross-thread engine reads — the game thread, the render thread, and what may be read across them

*An evolving reference for the other half of the threading problem. [Thread-safe
destruction](thread-safe-destruction.md) is about objects the game thread FREES under the render
thread; this page is about every engine field the render thread READS, or writes, while the game
thread owns it — which thread runs what, what the fence covers and what it does not, what x86
actually promises about a load of a misaligned engine field, and the audit of every such site as
of 2026-09-11. Engine addresses are* <span class="pill pill-ok">VERIFIED</span> *against
`pristine/TotalA.exe.pristine` (ImageBase `0x400000`) with `i686-w64-mingw32-objdump -d -M intel`
unless marked* <span class="pill pill-warn">INFERRED</span>*. First written 2026-09-11 from the
audit that followed the G13u landing review; see also **GPU status, hooks & limits** §3 (the
known-gaps table), **Reverse-engineering the exe** (the misaligned `main`, and the per-map array
census this page leans on), and **Sizing the wide fog grid** §2 (the one fault this class has
produced).*

## Status

| | |
|---|---|
| **Threads that touch engine memory** | two: the engine's own thread, which is the sim and every draw; and the fork's render thread, created at `dd.c:1354` |
| **Fence** | `tagpu_reclaim`'s wrap of the level teardown `0x491B60`, the pass counters, and the gate at `tagpu_overlay.c:590` — covers the level **teardown**, not the next level's **load** (§2) |
| **Audit** | the nine sites of the G13u sweep, each classified against the writer it reads (§5). **Every one of them is CLOSED as of 2026-09-12**: rows 1, 6, 7 and the fog half of 4 by landings 4a–4c, row 2 by landing 3, rows 8 and 9 by landing 2, row 3 — the particle heap, the one the fence never covered — by landing 4a. What is still read on the render thread is per-LEVEL ASSETS under the fence (model templates, FeatureDef and wreck records, the tile set, GAF frames), which is the deliberate residual [thread-safe destruction](thread-safe-destruction.html) §10b names and the plan's row 5 and the asset channel retire |
| **Open** | §7 |
| **Design** | the plan this audit led to, kept in the wiki as authored HTML: [Frame packet exchange](frame-packet-exchange.html) — one publisher on the game thread, one wait-free four-slot exchange, four reviews folded in on 2026-09-11. **Landing 1 built 2026-09-12** (`tagpu_packet.c`, `tagpu_packet_pub.c`): the primitive, the header packet, the marker font as glyph bytes, the out-of-game packet and the build rule in census mode. **Landing 2 the same day** ([GPU status](gpu-status.html) §2.17): the view (the eye, the true viewport, the rect the engine can name, the palette, the gamma) reaches the render thread only through the packet, and every render-thread STORE into engine memory became a command the game thread applies at the top of the in-play draw — the eye row of §4 is the first to convert, and no row is written across threads any more. The readers in §4 that walk per-map arrays are still on engine memory until landings 3–4 |
| **The loader thread** | the plan's engine review found it and the disassembly confirmed it on 2026-09-12: the level load runs on a thread created at `0x4982CA`, whose last act sets bit 1 of `main+0x38D75`; the in-play frame is installed only after that bit is seen, so the first publish after a load is ORDERED after the loader by the engine itself ([engine map](exe-reverse-engineering.html), "The in-play publish point"). That is what closes §5's open hazard once the unit array is read from the packet (landing 3) |

## 1. The two threads, and the one that is not

The engine is single-threaded. One thread runs the simulation, `DrawGameScreen`, every draw call
and every allocation — call it **the game thread**. It owns every byte under
`main = *(char**)0x511DE8`, and it turns `DrawGameScreen` over roughly 7–15× per presented frame
(443/s at 1920×1080 — [exe reverse engineering](exe-reverse-engineering.md)), so a field it
rewrites per draw is rewritten several times inside one of our frames.

The fork adds **the render thread**: cnc-ddraw's presenter, `g_ddraw.render.thread`, which once per
presented frame uploads the engine's surface and calls `tagpu_overlay_draw` (`render_ogl.c`) between
`tagpu_reclaim_pass_begin` and `pass_end`. Everything the fork draws itself — the native unit pass,
terrain, features, effects, markers, the UI layer, the writeback — runs there and reads engine
memory while the game thread is mutating it. It also **writes** a few engine fields: the eye and
the scroll target (behind `clamp_pair()`), the view rect through `tagpu_vpwide`, the minimap's view
rect. `gpu-status.md`'s *fields we write* table is the authority on those.

The fork's game-thread code is its detours: the `Game_MainLoopTick` detour at `0x4969D2` (the
reclaim drain, the scenario creation pass, the zoom follow release), the own-the-draw detours, the
teardown pre and post hooks, the gadget callbacks. Anything that runs from one of those is on the
game thread and reads engine memory exactly as the engine does — **no exposure**, whatever it reads.
`tagpu_order.c`, `tagpu_weapons.c`'s sim side and `tagpu_scenario.c`'s resolvers are in this class.

`fog_alarm_thread` (`tagpu_fx.c`) exists only to show a message box off the render thread. It reads
nothing.

## 2. The fence — what it covers, and the two holes

The mechanism is `tagpu_reclaim.c`'s, described in full in [thread-safe
destruction](thread-safe-destruction.md) §4–§6; what matters here is its *shape*:

1. **On the render thread**, `pass_begin` increments `s_started` and latches the teardown flag for
   the pass; `tagpu_overlay_draw` runs; the gate at `tagpu_overlay.c:590` replays the latch and
   returns before the first engine read if it was set; `pass_end` publishes `s_completed`.
2. **On the game thread**, the pre hook on `0x491B60` sets the flag and spins until
   `s_completed == s_started` — the reader is between passes — for at most `RC_TEARDOWN_WAIT_MS`
   (1000 ms), then lets the cascade free. The post hook, run the moment `0x491B60` returns, bumps
   the level generation and **clears the flag**.

**What the cascade frees under it** — every per-map array the render thread reads: the unit array
(`0x485980`), the particle layer table (`0x471DE0`), the minimap surfaces (`0x466AA0`), the map
(`0x483DD0`: tile set, tile map, feature map, fog descriptor and buffer, and through `0x422170` the
wreck records), the model templates (`0x42DB90`), the projectile array (`0x499A80`), and every
`Object3do` through `FreeObjectState`. The addresses and store sites are in the exe note's
*per-map arrays* section.

**What runs before the gate.** Eight `*_flush` calls — the tracer, suppressor, own-draw, effects,
features, terrain, markers and GUI flushes. Checked 2026-09-11: they are stat loggers and
90-frame skip watchdogs, and **none of them reads engine memory**, so their position before the gate
is not an exposure. The on-demand tooling that also runs before it does read engine memory —
though **since landing 10c-1 (2026-09-18) only one of the six still does so on the render
thread.** `tagpu_peek_frame`, `tagpu_weapons_frame`, `tagpu_ui_frame`, `tagpu_cat_frame` and the
scenario detection frame now run on the GAME thread, from the engine's own flip, so they cannot
fault the render thread at all: they were moved to reach `renderer=gdi`, and this fell out of it.
What is left is **the tracer's `sample_composites`** (`tagpu_tracer_flush`, still called from
`tagpu_overlay.c` on the render thread, `g_armed`-gated). It reads the unit array and
`Object3do+0x10`, never the UnitDef array. So a `tacli` **trace** issued **during a level change**
can still fault the render thread; that is a tooling hazard, not a play one, and it is now one
verb rather than four. So can `writeback_paint`'s opt-in 3DO path on the
`tagpu_overlay.off` exit, which also precedes the gate.

**Hole 1 — the timeout.** If the reader has not finished its pass within a second (a GL stall, or
a stuck render thread) the pre hook gives up. The reclaim queue is kept — that part is the "safe
leak" the note describes — but **the cascade's own frees proceed** against a reader that may still
be mid-pass, and everything in §4 becomes a live use-after-free for that one teardown. This is the
design's one timing-dependent mitigation, and it should be cited as one, never as a guarantee.

**Hole 2 — the load.** The flag clears in the post hook, so the next level's load runs with the
reader live: `0x471D90` layers, `0x499A30` projectiles, `LoadMap 0x483610`, `0x4854A0` the unit
array, `0x4669B0` the minimap. For every per-map field but one this is safe **by ordering**: the
teardown nulled the pointer under the fence, every reader refuses a null, and the load publishes the
new pointer in one store after the object behind it is complete — `LoadMap` even writes the map's
dimensions (`0x483881`/`0x483892`) before any of its pointers (`0x483969` onward), so a reader that
sees a new pointer sees the new dimensions. The one field published in **two** stores is the unit
array's `begin`/`end` pair, which is §5's finding.

## 3. What x86 promises about a load of an engine field

**Our own statics are aligned, so one dword is one atomic load.** That is the compiler's doing and
it is why `tagpu_zoom.h`'s published view, `tagpu_reclaim`'s counters and every fork-side flag can be
read across threads as single words.

**No engine field is.** `0x41D920` pads the allocation of `main` by `7 · (GetTickCount() % 1000)`
and stores `base + pad` into `0x511DE8` — verified instruction by instruction, [exe reverse
engineering](exe-reverse-engineering.md). So every field's alignment is drawn afresh at each launch
and then fixed for the session. What that costs, for a dword at `main+N` with `N ≡ 3 mod 4` (every
offset this page is about):

| | share of launches |
|---|---|
| 4-aligned | 25 % |
| tear-capable on **Intel's** documented guarantee — the dword crosses a 64-byte cache line (SDM 3A §8.1.1: anything inside one line is atomic on P6 and later) | ~4.7 % |
| tear-capable on **AMD's** documented guarantee — the dword crosses an aligned 8-byte boundary (APM vol. 2 §7.3.2: "naturally atomic … as long as they do not cross an aligned 8-byte boundary") | 37.5 % |
| misaligned at all | 75 % — not a tearing condition on either vendor |

The reference setup is an AMD part, so the honest figure here is the 8-byte one. Whether a Zen
core actually splits an access inside a cache line has not been measured; assume the documented
guarantee. Until 2026-09-11 the notes quoted "about one launch in twenty" (Intel's figure) and
called the 75 % "load-bearing" (it is not).

**Tear versus skew, and why the skew is the one that matters.** A *tear* is one load observing half
of one value and half of another; it needs a tear-capable launch, and the load and the store must
coincide. A *skew* is two loads observing values from different generations — `begin` from this
game, `end` from the last — and it needs nothing: two loads are never atomic as a pair, aligned or
not, in 100 % of launches. Every multi-word engine read (`{begin, end}`, `{buf, cols, rows, cells}`,
`{w, h, pitch, base}`) has the skew hazard first and the tear hazard as a rarer variant of the same
outcome. An audit that starts from residues mod 4 is looking at the wrong thing; start from the
writer.

**What a tear can and cannot produce.** A tear composes bytes of the old value and bytes of the new,
at one split offset. Two valid userland pointers, or a pointer and zero, never compose `0xFF` in the
top byte. That is why the 2026-09-03 fog faults — a `buf` of `0xFFFFFFF7` and of `0xFFFFFEC2` — are
not a tear ([sizing the wide fog grid](fog-grid-sizing.md) §2) but a freed-and-reused descriptor,
i.e. a lifetime fault; the guard commit (`55bc8e5`, 09-03) predates the teardown fence (`2062b19`,
09-06) by three days, and the descriptor is freed only inside that cascade. Consistent with the
lifetime reading; not proof of it. The one-fault test that would settle it is still §7's.

**The rule.** A torn *coordinate* is survivable when its consumer bounds it, which is why the eye
and the scroll target are written from the render thread behind `clamp_pair()`. A torn *pointer,
length or index* is a wild read, and no reader-side check closes it: `ptr_ok` filters a value,
`IsBadReadPtr` answers a question about the past, and a `lock` prefix on our store does nothing for
the engine's own plain split load. The only arguments that count are the ones `CLAUDE.md` names — a
bound the consumer applies to DATA, a lifetime (the fence, or reclaim), or a thread (do it from the
game thread).

## 4. The census — what the render thread reads, and who writes it

Reader = the render thread unless stated. "Fenced" means freed only by the teardown cascade under
the pre hook's wait, and republished by the load in one store. The engine addresses are in the exe
note's *per-map arrays* section.

| Field | Written | Lifetime class | Fenced? | The reader's own bound |
|---|---|---|---|---|
| `main+0x14357`/`+0x1435B` unit array `begin`/`end` | `0x4854A0` at load, in **two stores** around a full memset; `0x485980` nulls `begin` only | per map | teardown yes, **load no** (§5) | **NOT READ ON THE RENDER THREAD since 2026-09-12 (landing 3): the source is the packet.** The publisher walks to the engine's own slot count instead of reading the pair at all |
| unit records (the slots themselves) | the sim, every tick | Mode B: a fixed array recycled in place | n/a | **source after landing 3: the packet.** Every value out of a slot is still bounded before use, and now in ONE place — `ModelId` by `UNITINFOCount`, the cargo links to packet indices, the shape [thread-safe destruction](thread-safe-destruction.md) §2 describes |
| `Object3do` behind `unit+0x9E` | freed per death | Mode A | reclaim's first client | **NOT READ ON THE RENDER THREAD since landing 3**: the piece poses, the body turn, the base piece and the composite's rect are copied by the game thread. The address crosses as an opaque cache key. This is what makes reclaim's deferral redundant *for the render thread's unit reads* — it still covers the model templates |
| model templates (and the FeatureDef and wreck records beside them) — **the fence is the argument only while `tagpu_reclaim` is ARMED; [thread-safe destruction](thread-safe-destruction.html) §10b names the residual when it is not** | freed by `0x42DB90` in the cascade | per map | reclaim's second client + the level-generation caches | still read on the render thread, deliberately: the fence IS the argument, and landing 3 did not change it. The packet carries the per-piece template node and the BOUND (`udef_count`); the `MODEL_PTRS` BASE is read live at every call, because the teardown frees it at `0x42DCCB` and nulls `main+0x14377` at `0x42DCD8` — that null is what refuses the walk, and a copy held for a frame would not see it. **[CORRECTED 2026-09-12 by a landing review, which found the base cached in the packet.]** |
| `main+0x1421F` fog descriptor `{buf, cols, rows, cells}` | `LoadMap` once; the builder `0x4843C0` rewrites **cells** per draw | per map | yes | **NOT READ ON THE RENDER THREAD since 2026-09-12 (landing 4b): the source is the packet.** The publisher checks the same relation `cells == ((cols·rows + 7) & ~7)` on the thread that runs the builder, copies exactly `cols·rows` entries, and the acquire checks the area's length is exactly that — so the consumer's bound is its own bytes rather than a dimension cap |
| `main+0x1428B` tile map, `+0x14287` feature map, `+0x14233`/`+0x14237` dims | `LoadMap`, dims before pointers | per map | yes | **the FEATURE map is not read on the render thread since landing 3** — its cells are per-TICK sim state, so the anchors of the widest zoom rect cross in the packet with the six height bytes the two height rules need. The tile map is the terrain pass's and unchanged; the dims come from the packet |
| `main+0x14283` tile set `{count, pixels}` | `LoadMap` | per map | yes | `count ≤ MAX_TILES`, identity test before the megabyte probe |
| `main+0x1420B` wreck records | one direct store, in the feature teardown the cascade reaches | per map | yes | the index is the ANCHOR's now, and the anchor came out of the packet; the 3D husks' poses are copied by the publisher, so only the feature pass's GAF wreck still reads a record |
| `main+0x141F7` projectile array, `+0x141F3` live count | array at load, **300 slots** of `0x6B`; count by the sim at 13 sites | array per map, count per tick (Mode B) | yes | **NOT READ ON THE RENDER THREAD since landing 4a: the source is the packet.** The publisher's walk is bounded by the allocation — 300, which both of the engine's own append sites also refuse to exceed — so the 8192 cap is gone. The explosion records (`main+0x1491F`, inline, 300) and the 100 debris slots came with them |
| `main+0x38D77` particle layer table; each layer's `{begin, end}`; each object's sub-vector | table at load; the vectors **grow mid-play**, freeing the old array (`0x4732E0`) | table per map; vectors Mode A | table yes, **vectors no** | **CLOSED 2026-09-12 by landing 4a — this was the one row no fence covered.** The walk is the game thread's now, which is the thread that grows those vectors, so the pair cannot be skewed and a consistent pair cannot name freed memory. The engine's own rule is the cap plus one a layer — 401 in stock, 20 481 under the raised limits (past the cap the emitter drops the front and shifts), and every drawable sub-particle crosses as 16 bytes with its GAF frame already resolved |
| `main+0x142DB`/`+0x142DF`/`+0x142E3` minimap surfaces `{w, h, pitch, base}` | the minimap build at load; pixels repainted per draw | per map | yes | **NOT READ ON THE RENDER THREAD since landing 4c: the source is the packet.** The same cross-checks moved to the publisher, which does the three-way interleave the render half used to do per frame and per row — gated on the sharp minimap asking, because at k = 1 it is deliberately the engine's own minimap |
| `main+0x37E37`/`+0x37E3B` view W/H | the engine's setter `0x49821D`/`0x498237`; **and the render thread**, through `tagpu_vpwide` | per resolution change | n/a | consumers fail closed on a zero; the lost-update case is detected and repaired (`vpwide: REPAIRED`) |
| `main+0x1431F`, `+0x142F3`/`+0x142F7` eye, scroll target, followed object | the camera stepper, per draw; **and, until 2026-09-12, the render thread** for the first two | per draw | n/a | coordinates behind `clamp_pair()`; the followed-object *pointer* is released from the game thread for exactly this reason (G13u). **Source after landing 2: the packet** for every render-thread reader of the eye, and the game thread's command apply for every write — the row no longer crosses a thread in either direction |

**What landing 1 of the exchange changed here (2026-09-12).** None of the rows above moved: the
header fields the packet carries (the eye, the scroll target, the true viewport, the option bits,
the tick and the live speed, the counts) are not in this table because the audit's census was of
per-map *arrays*, and the one render-thread read that did go — the marker font behind
`[globals+0x204]`, dereferenced per glyph behind `IsBadReadPtr` — was not in it either, because
no note had established the font's lifetime. It now travels as bytes copied at hook 8.

**What landing 2 changed here (2026-09-12).** The eye row converted, in both directions: the
render thread reads the eye (and the true viewport, the rect the engine can name, the palette and
the gamma) from the packet and writes nothing into `main` — its camera writes are a command
record the game thread applies at the top of the in-play draw ([GPU status](gpu-status.html)
§2.17). §3's tearing arithmetic therefore no longer has a render-thread *store* to apply to; the
remaining cross-thread loads are the per-map arrays below, landings 3–4. The follow release is
now part of the same game-thread apply rather than a request consumed by terrown's fog tick.

## 5. The audit of the G13u sweep — nine sites, one open hazard

The sweep of 2026-09-10 listed nine places where a field written by one thread is read by the
other, and rated the unit array the headline. Each row below was checked against the writer, not
the reader.

| # | Site | Verdict | Why |
|---|---|---|---|
| 1 | fog descriptor, captured once and held for the frame (`tagpu_native.c`, used by `tagpu_fog_at`) | **CLOSED 2026-09-12** by landing 4b; the sweep's mechanism was wrong and the row was never a live hazard for the reason it gave | nothing resizes the grid between map loads: the descriptor is built once by `LoadMap`, freed only by the fenced cascade, and the builder rewrites cells only. What landing 4b changes is that the descriptor and its bytes cross as ONE record whose length the acquire proves, so `tagpu_fog_at`'s guard — added after a hard fault off a base of −9 on 2026-09-03, **root cause never found and still not found** — is guarding a bound that now holds by construction |
| 2 | unit array `begin`/`end` | **CLOSED 2026-09-12** by landing 3 of the [frame packet exchange](frame-packet-exchange.html) — it was real, unfenced and alignment-independent; the level-load skew is still described below because it is what had to be closed | no render-thread file reads the pair, or the array, or any Object3do. The game thread copies the units into the packet during an in-play draw, and its walk runs to the engine's own SLOT COUNT (`u16 main+0x14351`, stored *before* `begin`), so there is no pair to skew. The exact relation `end == begin + (count−1)·0x118` is asserted per publish for the record (`relbad=` on the heartbeat, 0 over every run) and bounds nothing |
| 3 | particle layers and sub-vectors (`tagpu_sfx.c`) | **CLOSED 2026-09-12** by landing 4a — it was real and it was the one row no fence covered | the vectors grow mid-play and free the old array, so no read-side gate could close it; what closed it is that the walk moved to the thread that grows them. The probes and the `ns ≤ 4096` filter went with it (a containment filter of the same shape stays in the publisher, and says so). It is no longer reclaim's next client: there is nothing left to defer |
| 4 | tile map / feature map + dims (`tagpu_terr.c`, `tagpu_feat.c`) | not a skew hazard | `LoadMap` stores the dims before the pointers, the cascade nulls the pointers under the fence, every reader loads the pointer first — so a reader that sees a non-null pointer sees the matching dims. The "2048×2048 off a smaller allocation" sequence needs the opposite store order |
| 5 | tile set `{count, pixels}` | same as 4 | same routine, same order, same null |
| 6 | projectiles `np`/`pbase` | **CLOSED 2026-09-12** by landing 4a | the walk is the game thread's; the cap IS 300 now, and it is the allocation's rather than a number typed here — `0x499A30` allocates exactly 300 slots and both append sites refuse past 300 |
| 7 | minimap surfaces (`tagpu_gui_surf.c`) | **CLOSED 2026-09-12** by landing 4c; it was not a live hazard, and the comment was wrong where the code was right | per map, fenced, dims and pitches cross-checked. The old comment's "the worst a torn read can do is put one frame's fog against another's" was true of the pixels and false of the descriptors, which carry a pointer and a pitch. All three now cross as one interleaved copy the publisher makes |
| 8 | view W/H (`tagpu_vpwide.c`) | survivable | both threads write it; a torn dimension fails closed; the lost update is repaired |
| 9 | eye / scroll target (`tagpu_zoom.c`) | documented | coordinates, bounded before the write |

Residuals common to every fenced row (1, 4, 5, 6, 7): the timeout of §2, and a torn single-pointer
load at the null-to-new publish, which needs a tear-capable launch *and* the load coinciding with the
store. Negligible rate; not zero; named here so nobody has to re-derive it. **After landings 4a–4c
those residuals apply to the PUBLISHER's reads, on the game thread, where the store and the load are
the same thread and neither can happen.** What is left on the render thread is the per-LEVEL asset
class — model templates, FeatureDef and wreck records, the tile set, GAF frames and the cursor
table — which is not in this table because it was never per-frame state, and whose retirement is
the plan's row 5 and the asset channel after it ([thread-safe destruction](thread-safe-destruction.html) §10b).

### The unit array at level load

The sequence, from the binary:

1. **Teardown** `0x485980`, called from the cascade at `0x491B95`: `MEM_Free` the array
   (`0x485A17`), `begin = 0` (`0x485A27`). `end` is not touched — `0x4855D6` is the **only** store
   to `main+0x1435B` in the binary.
2. The post hook clears the fence. The menus run with the reader live; every reader refuses the
   null `begin`.
3. **Load** `0x4854A0`, called from the level-load routine at `0x4918D4`: the slot count
   `u16 main+0x14351 = 10·[main+0x37EE6] + 1` (`0x4854EF`); `MEM_Alloc` of `count·0x118`;
   **`begin` stored** (`0x485525`); `rep stos` over the whole array; two more allocations;
   **`end = begin + (count − 1)·0x118` stored** (`0x4855D6`).

For the length of that memset the pair is (new `begin`, last game's `end`). **Six render-thread
readers had it, and landing 3 is what removed all six** — `tagpu_native_frame` and
`tagpu_scaffold_frame` read the packet's units table, `log_units` read it too (and since the
vulkan-only plan's landing 10c-3 it is not on the render thread at all — it is `roster_log` in
`tagpu_packet_pub.c`, on the game thread, so that `renderer=gdi` has a roster),
`probe_unit_model` and `writeback_paint` are deleted, and `tagpu_mark_gather` walks the table. A
seventh, `tagpu_order.c`'s `sane_unit`, bounded a record's unit pointer against the same pair on
the present thread and is now a binary search over the packet's own table by array slot. **The
argument is the engine's own ORDERING, not a copy that outruns the load**: the level load runs on
the loader thread, whose last act sets bit 1 of `main+0x38D75`, and the in-play frame callback —
the only site the publisher acts on — is installed only after the game thread has seen that bit
([engine map](exe-reverse-engineering.html), "The in-play publish point"). So the publisher cannot
run while the array is being built.

The paragraph below is the hazard as it stood. All six gated only on
`ptr_ok(beg) && ptr_ok(end) && end > beg` plus the 20000-slot cap and then walked
slots with no per-slot probe. If the allocator hands back the same
block the skew is invisible. If the new block lands below the old end, the walk runs past the new
allocation into memory the teardown freed — which on the Wine heap can be unmapped, the note's own
Mode A premise. Nothing here has faulted; the rate is per game start × the memset's share of a
frame × heap luck. A torn load of `begin` alone, in a tear-capable launch, is the rarer variant.

**On "a relation is a filter, not a fix".** The `(end − begin) % 0x118 == 0` relation is a filter:
two unrelated allocations pass it one time in 280. The **exact** relation
`end == begin + (count − 1)·0x118`, with the count the engine publishes *before* `begin`, is not a
filter against a skew — a skewed triple passes it only when the skew is benign (same block). It is
still only a filter against a torn load, since a torn value can coincidentally satisfy it. So it
belongs as a refusal layer, and the lifetime argument closes the tear.

**The by-construction options.** (a) The shape `tagpu_reclaim` already applies to `0x491B60`,
applied to `0x4854A0`: a pre hook that sets the flag and waits for pass completion, a post hook that
clears it — a hold of the order of the memset, once per game start; whether `0x4854A0`'s entry
bytes detour cleanly is unverified. (b) The same post hook publishing `{begin, end, generation}`
into aligned fork statics, with the readers taking the pair from there: closes skew and tear both.
(c) Moving the existing flag's release from the teardown's post hook to the first sim tick of the
new game, which the destruction note once described as the design: it covers the load with one
change but blanks the UI layer for the whole time between games. Neither is done; the reads carry
a comment saying so.

## 6. Rules for a new cross-thread read

1. **Which thread?** A detour or a gadget callback is the game thread — no exposure. The overlay is
   the render thread — everything below applies.
2. **Name the writer and its lifetime class** before writing the read. Per session (never
   rewritten): read freely. Per map: nulled by the cascade under the fence and published by the load
   — check the publish is *one* store and that anything the pointer's consumer needs (dims, counts)
   is stored *before* it. Per tick, in place (Mode B): every value out of it is DATA and gets a bound.
   Grown or freed mid-play (Mode A): it needs reclaim, or it is exposed and the comment says so.
3. **A multi-word read gets a relation the builder guarantees** — exact where one exists (`end` from
   `begin` and the count; `cells` from `cols·rows`) — as a refusal layer. It is never the argument.
4. **A pointer, length or index cannot be made safe on the reader's side.** The argument is a
   lifetime or a thread. A coordinate can: bound it where it is consumed.
5. **`ptr_ok` and `IsBadReadPtr` are nets.** Keep them; say so in the comment; never cite them as the
   reason a read is safe.
6. **Nothing that reads engine memory runs before the gate** unless it is trigger-gated tooling
   — since landing 10c-1 that means `tagpu_tracer.c` alone, the other five having moved to the
   game thread —
   and then its `tacli` documentation should say "not during a level change".
7. **A change to any of this is a `high` review** (`CLAUDE.md`, *Review engine changes*), and the
   second reviewer's brief is the sequence that breaks the claim, not an opinion on the design.

## 7. Open items

- ~~**The unit array's publish at level load** (§5) — the one open hazard.~~ **CLOSED 2026-09-12**
  by landing 3 of the frame packet exchange. Not by any of the three options this note listed: the
  readers went instead, and the ordering the loader thread already provides became the argument.
  The exact relation is still checked, as the option would have had it, but as a RECORD — nothing
  is bounded by it, because the walk that replaced them runs to the count.
- ~~**The particle layers' vectors and the per-object sub-vectors** — reclaim's catalogued next
  client.~~ **CLOSED 2026-09-12** by landing 4a, and not by deferring anything: the walk moved to
  the thread that grows those vectors, so there is nothing left for reclaim to defer. It was the
  only row in §4 that no fence covered.
- **The timeout** — a timing-dependent mitigation that the cascade's own frees fall through. Either
  accept it explicitly, per `CLAUDE.md`, or defer the cascade's remaining frees the way the
  templates' are deferred (§6c of the destruction note), which turns the timeout into a leak rather
  than a fault.
- ~~**The projectile cap** — `8192` in `tagpu_fx.c` against an allocation of 300.~~ **CLOSED
  2026-09-12** by landing 4a. It is 300, in the publisher, and it is the allocation's own number:
  `0x499A30` allocates exactly 300 slots and both of the engine's append sites refuse past 300.
- ~~**Tooling before the gate** — document the level-change restriction in the `tacli` skill, or move
  the trigger frames below the gate at the cost of not serving triggers during a teardown.~~
  **CLOSED for five of the six since 2026-09-18** (landing 10c-1), and by a third option this item
  never listed: the trigger frames moved to the **game thread**, off the engine's own flip, so they
  are not before the gate because they are not on that thread at all — and they serve triggers
  during a teardown, which both of the options above would have cost. `tagpu_tracer.c`'s
  `sample_composites` is the remaining one and still wants the `tacli` note.
- ~~**The one-fault test for the fog guard** — plumb `cells` into `fog_alarm` so the next trip logs
  all four descriptor fields.~~ **MOOT since 2026-09-12** (landing 4b): both grids cross in the
  packet with their dimensions, and the acquire checks that the area's length is exactly
  `cols·rows·2`, so the bound the guard exists to enforce holds by construction. The guard is kept
  — three comparisons — but a trip now means the packet validator has a hole, which is a different
  and louder thing than a torn descriptor. **The 2026-09-03 fault's root cause was never found and
  still has not been**; what is gone is the class, not the diagnosis.
- **Zen's split behaviour** — unmeasured; the documented 8-byte guarantee is assumed.

## Changelog

- **2026-09-12 (later)** — §5's row 2 and its open item CLOSED by landing 3: the six readers are
  converted or deleted, the seventh (the order markers' `sane_unit`) with them, and the census's
  "source after" for the unit array is the packet. §5's description of the hazard is kept as the
  record of what was closed.
- **2026-09-12** — the loader thread row in the status table and the landing-1 note under §4;
  the design's first landing is built, the readers are unchanged.
- **2026-09-11** — first version, from the audit of the G13u sweep. Corrected the same day in
  three other notes: the fog grid is per map and under the fence (destruction note §3), the fence
  clears in the post hook and not at the next live game (§6), and the alignment figures are
  vendor-dependent, with the 75 % not a tearing condition (exe note, GPU status).
