# Thread-safe destruction — deferring engine frees the render thread still reads

*An evolving reference for one recurring hazard: the fork's async GL render thread dereferences an
engine object that the game thread frees underneath it. This page records the confirmed crash, the
reusable pattern that closes it, the object catalogue it applies to, and the gates it must pass
before landing. The first client is **built and measured** (G14h); the page stays the reference
for the pattern and its next clients — update it as they land. Engine addresses are* <span class="pill pill-ok">VERIFIED</span> *from `TotalA.exe`
(ImageBase `0x400000`) unless marked* <span class="pill pill-warn">INFERRED</span>*. First written
2026-09-06 from six parallel investigations; see also **GPU status, hooks & limits** §3.2 (the crash
as a known limit), **Memory Manager Investigation** (the heap this reclaims into), **Own the draw**
(the detour discipline reused here), and **Effects** / **Features** (the other objects the render
thread reads).*

## Status

| | |
|---|---|
| **Problem** | <span class="pill pill-ok">CONFIRMED</span> cross-thread use-after-free, `200v200` ~95 s in, ~2 of 3 runs |
| **Pattern** | **built** 2026-09-06 as `tagpu/ddraw/src/tagpu_reclaim.c` (G14h), on by default, `tagpu_reclaim.off` disables — see **GPU status, hooks & limits** §2.7 for the landed shape and the log lines |
| **First client** | the model object `Object3do` (units + wrecks + features), via `FreeObjectState 0x45AAA0` — landed |
| **Second client** | the **model templates** and their pointer table, via the two `MEM_Free` call sites inside `0x42DB90` — landed 2026-09-09, §6c |
| **Next clients** | the two particle heap surfaces (sub-vector, layer array); the fog grid separately |

## 1. The answer in one paragraph

Do not read around the free on our side. Cooperate with the engine's own destructor: intercept it,
enqueue the pointer instead of freeing, and let the real free run only after a grace period that
proves no render pass can still hold the pointer. The engine stays the owner of every object; we
change only *when* the physical memory returns to the heap. Its view of the world is unchanged, so
the simulation is untouched. This is epoch-based deferred reclamation (the read-copy-update / QSBR
idea), and it is engine-cooperative because the engine still nulls its own pointer and clears the
alive bit exactly as before, while the async render thread gets a guarantee it never had: the memory
it gathered this frame outlives the frame.

## 2. The organising principle — classify by how the engine reclaims the object

The crash only happens when freed memory becomes *unreadable*. On the wine growable heap a block
returned to the heap can be decommitted or unmapped. So every pointer the render thread dereferences
falls into one of two classes, and each gets one treatment:

- **Mode A — DEFER.** The object is returned to the heap (`MEM_Free 0x4D85A0`). A stale read can hit
  an unmapped page and fault. These need the deferred-reclamation pattern.
- **Mode B — ACCEPT.** The object lives in a fixed array, an arena, or a pool never returned to the
  OS during a match; a dead slot is recycled in place. A stale read is a wrong-but-mapped value —
  at worst a one-frame cosmetic glitch, never a crash.

  **That promise is conditional, and the condition is the part that gets forgotten:** every value
  taken out of a Mode B object is arbitrary, so it is DATA until it has been bounded as data. An
  index must be checked against the array's own count, a length against its allocation, a tag
  against its enum — and only then may it be used. `[MEASURED 2026-09-08]` the counter-example is
  what an unbounded one costs: `tagpu_native.c`'s Classic++ shadow pass took the unit record's
  `ModelId` (`unit+0xA6`, a Mode B read) straight into the model-template table with no bound at
  all, so a slot recycled between the frame's gather and the shadow loop addressed memory past the
  end of that table, and the pointer read from there walked as if it were a model — an access
  violation in `aabb_walk` at `fild [ebx+0x10]`, `EBX = 0x3D1E4B1E`, seven levels into the
  recursion, in a 400-unit game. Bounded (`1 <= mid < UNITINFOCount`, the range the engine's own
  loops use — [exe reverse engineering](exe-reverse-engineering.html) §`0x42D5xx`), the same stale
  read costs one frame of the wrong model's AABB, which is exactly what Mode B promises.

  **A range test is not that bound, and neither is a readability probe.** `ptr_ok` filters a
  pointer VALUE and is worth keeping as a cheap sanity net; it admits a 2 GB window, so it is
  never the argument. `IsBadReadPtr` answers a question about the past — the page can go away
  between the check and the read — so it makes a fault rarer without making it impossible. The
  fork uses it widely in the older passes; none of those uses is a safety argument, and new work
  does not add more (`CLAUDE.md`, *Fixes must be safe by construction*).

That single rule tells you, per object type, whether it needs the pattern.

## 3. What actually needs the pattern (the catalogue)

| Object | Class | Destroyer | Status |
|---|---|---|---|
| **Model object (`Object3do`) — units + wrecks + features** | A | `FreeObjectState 0x45AAA0` (callers `0x486D9E` unit, `0x42474F` wreck, `0x4221C4` bulk) | the confirmed crash — fix first |
| Its posed vertex buffers | A | freed *inside* `FreeObjectState` (per-prim loop at `0x45AAB2`) | covered for free by deferring `FreeObjectState` |
| Its composite frame (`obj+0x10`, read at `tagpu_native.c:1786`) | **unclassified** | **not** freed by `FreeObjectState` — `0x437C90` only zeroes a registry entry and makes no call; the frame's owner is the composite draw context at `*(TA+0x1437B)` <span class="pill pill-warn">INFERRED</span> | separate lifetime — classify Mode A/B before landing (§10) |
| **Model templates (`Model3DONode`) and their pointer table** — per TYPE, one block each | A | `0x42DB90` in the teardown cascade: `MEM_Free 0x4D85A0` per model (`0x42DC01`) and once for the table (`0x42DCB6`) | **landed 2026-09-09** — both call sites redirected, §6c |
| **Particle sub-vector** (smoke/fire/nano per-object point list) | A | each particle destructor `MEM_Free`s `obj+0x10` (nano `0x471560`, fire `0x4716A0`, smoke2 `0x474D10`, smoke1 `0x475110`, flare `0x471430`, wake `0x4717E0`) | latent UAF — same mechanism, fold in next |
| **Particle layer pointer-array** (during growth) | A | `std::vector` grow `0x4732E0` frees the old array | latent UAF, only while a layer grows |
| **Screen fog grid** | A | reallocated mid-frame; the indexed read has no `IsBadReadPtr` | second, independent hazard — wants a per-frame snapshot, not this detour |
| Projectiles, explosions, flying debris, the 76-byte particle objects, unit slots, wreck records, unit/feature defs, model templates, main-block fields | B / stable | fixed arrays, arenas, pools, per-map or session lifetime | already safe; a stale read is at worst one wrong frame |

So the pattern must cover exactly the model object now (the crash), and the two particle heap
surfaces as a follow-up. Everything in the projectile and effects passes is already crash-safe. The
fog grid is a separate class-A hazard with a different lifetime (rebuilt each frame), best closed by
copying it once per frame rather than by a destructor detour.

## 4. The mechanism

**One interception covers all three model-death paths.** Unit death, wreck death, and the bulk
feature teardown all call `FreeObjectState 0x45AAA0`. It is `__stdcall`, one argument, single exit
(`ret 4`), and its first five bytes `53 8B 5C 24 08` are a clean steal ending on an instruction
boundary (resume at `0x45AAA5`); nothing branches into that range and the body never re-enters
itself. A single detour at its entry catches every caller, and a trampoline over the stolen bytes is
how the drain calls the real free without re-entering the detour. It frees each prim's posed vertex
buffer (the loop at `0x45AAB2`) and then the block itself, so deferring the one function keeps the
object and its posed geometry alive together. It does **not** free the composite frames: the two
calls to `0x437C90` on `obj+0x10`/`obj+0x14` walk a `{ptr,size}` registry at `*(TA+0x1437B)` and
zero the matching entry — the routine contains no call at all. The frame memory has a separate owner
(the composite draw context), so its lifetime is a distinct question (§10).

**The flow.**

1. The detour, on the game thread, pushes `{object, current_epoch}` onto a fixed ring and returns
   without freeing. The engine's caller then nulls its own record pointer (`unit+0x9E` at
   `0x486DA3`, `feat+0x4` at `0x424754`) and clears the alive bit, exactly as before. Between the
   enqueue and that null the object is deferred-but-valid, so a gather landing in that window reads
   live memory.
2. The render thread bumps the epoch once at the top of its pass, before the gather.
3. After its last dereference of any gathered object, the pass is done reading; the queue is drained,
   freeing (through the trampoline) every entry whose epoch is far enough behind the current one.

**Why it is correct (quiescence, not a fixed pass count).** The render thread is the only reader, and
reads gathered objects only inside one pass (`tagpu_native_frame`); the gather buffer is refilled
every pass, so no pointer survives into a later pass. The object becomes unreachable to new gathers
at the engine's null of the record (`unit+0x9E` / `feat+0x4`), just after the free returns. The
rigorous rule is to free an object only once the reader has *published completion* of every pass that
could still hold it — an observation of the reader's own progress, never a wall-clock or fixed-count
assumption. Concretely, the reader publishes two monotone counters, **pass-started** and
**pass-completed**; between them it may hold pointers it gathered this pass, outside them it holds
none. The drain runs on the game thread, so it executes *after* the null; it snapshots pass-started,
and frees the object only once pass-completed has **reached** that snapshot (`completed ≥ snapshot`,
not `>`: an idle reader has `completed == started`, so it satisfies the test at once — with a strict
`>` every death would leak while the game sits idle or minimised). A pass that starts after
the snapshot began gathering after the null and cannot have the object; every earlier pass is
finished once completion passes the snapshot. So no reader can hold a pointer the drain frees —
regardless of how late either thread runs.

**If either thread is late it stays safe.** A stalled reader simply freezes pass-completed, so
nothing is freed; the ring fills and the overflow leaks, which is harmless. The design never frees
under doubt. That is the guarantee: correctness rests on observing the reader's actual progress, not
on anyone being on time.

**Why a fixed "free N passes behind" is *not* a proof.** It starts the grace clock at the wrong
moment — when we intercept the free, one instruction before the engine's null — and it assumes the
null lands within N−1 reader passes of the stamp. The failure is on the producer, not the GPU: if
the game thread is preempted at the free-to-null gap while the render thread races ahead more than
N−1 passes, a later pass gathers the object after the fixed grace has expired, and the drain frees it
underneath that pass — the use-after-free again. Use the published-completion snapshot above, which
carries no timing assumption; a fixed count is at best a probabilistic margin.

**No hot-path locks, one fence total.** On x86 the ring is a standard single-producer /
single-consumer queue needing only a compiler barrier: the producer writes the slot then publishes
the tail; the consumer reads the tail then the slot; hardware store/load ordering does the rest.
Stale head/tail reads are always conservative (look fuller or emptier), never corrupting. The only
real hardware fence in the whole design is the teardown handshake (§6).

## 5. Which thread runs the free — drain on the game thread (recommended)

The real `FreeObjectState` walks a global animation manager (`0x437c90`, reading
`[ds:0x511de8 + 0x1437b]`) while freeing the composite frames. If the drain ran on the render thread
it could touch that global while the game thread mutates it. The clean shape is to keep the
*decision* on the render thread (it publishes the epoch) but run the actual free on the **game
thread**, from the `Game_MainLoopTick` detour that already exists at `0x4969D2`. Then every real free
happens on the same thread the engine always freed on, so no engine global is touched cross-thread,
and the grace guarantee only strengthens (the free happens later still). This removes the one
residual risk in the single-thread-drain variant.

## 6. Teardown safety

On level change the engine bulk-frees game state at `0x491B60`, and that cascade is the only path
that reaches the non-nulling bulk `FreeObjectState` caller `0x4221C4`
(`0x491B60 → 0x483DD0 → 0x422170 → 0x4221C4`, each reachable only from the one above). A deferred
queue must not free into a heap being torn down. Detour `0x491B60` at entry (first five bytes
`A1 E8 1D 51 00`, a clean steal, resume `0x491B65`): set a teardown flag, wait briefly for the render
thread to leave its pass (a short handshake using interlocked stores on the flag and a render-thread
"busy" flag — the one place a hardware fence is required — with a timeout that degrades to a safe
leak if the render thread is stuck), flush the queue while the reader is idle, then let the cascade
free normally. The tick detour clears the flag on the next live game. While the flag is set, any
death frees synchronously, which is safe because the reader is quiesced.

### 6a. The level generation — what the deferral does NOT cover

`[BUILT 2026-09-08]` `FreeObjectState` owns one lifetime: an `Object3do` and the posed vertex
buffers hanging off it. It does not own the **model templates**, and they are not merely
unclassified — **`0x42DB90` frees them**, called from the teardown cascade at `0x491C21`.
`[BINARY-VERIFIED 2026-09-08]` it walks the model-pointer table `main+0x14377` bounded by
`main+0x1438F` (`esi` runs over the unit defs at `main+0x1439B`), `MEM_Free 0x4D85A0`s each entry
(`0x42DC01`) and nulls its slot (`0x42DC15`), then frees the table itself (`0x42DCB6`) and nulls
`main+0x14377` (`0x42DCD8`). Those blocks go back to the same allocator whose small-block heap
this note already documents as recycling aggressively, so a render-thread cache keyed on a
template pointer is not protected by any of the machinery above **and** the address-reuse premise
is not hypothetical.

`tagpu_reclaim_level_gen()` is the counter that closes the CACHE half of it (the block half is
§6c, which defers those frees so a walk in flight cannot be reading freed memory in the first
place): `InterlockedIncrement` in the
**post hook**, after the cascade has freed the templates and before the reader is released.
Unconditional — the pre hook's `busy` path keeps the queue and frees nothing of ours, but
`0x42DB90` runs either way. A cache stamps its entries with the generation and drops them when it
changes; the counter never moves on an exe where the teardown could not be hooked, which is the
behaviour those caches had before it existed.

**It must not be bumped in the pre hook, and this is a trap rather than a preference.** The render
thread is not stopped by `pass_begin` — `render_ogl.c` ignores its return — it is stopped by
`tagpu_overlay.c`'s `teardown_active()` gate at line 586, and `tagpu_native_frame` is thirteen
lines further on, past `log_units` (file I/O) and the scaffold. A pass that cleared that gate
before the flag was set runs on while the pre hook waits for it, so a generation bumped there is
observed by a frame that then drops the caches **and refills them from templates the cascade has
not freed yet** — stamping the new generation onto stale entries, which are then never dropped
again. The first draft of this landing did exactly that; the landing review caught it.

Its first client is `tagpu_native.c`'s three template caches — `s_aabb` (the whole-tree AABB the
shadow height rule reads), `s_sbox` (the select box's bounds) and `s_pmap` (glTF piece → engine
primitive). None of them was dropped by anything before this. Measured on a real teardown
(surrender → main menu, Two Continents, one ARMCOM selected):

```
reclaim: level teardown (gen 0 -> 1): flushed 1 queued object(s), reader idle; the cascade frees synchronously
native: level 0 -> 1, dropping the template caches: aabb=1 selbox=1 pmap=0
```

### 6b. **The teardown wrap freezes the game on quit-to-menu** — open, pre-existing

`[MEASURED 2026-09-08]` Surrendering a skirmish (`Tab → EXIT → MAINMENU → CHOICE1`) **hangs the
game while this module is armed.** The teardown line is written, a few more frames run, and then
the process stops: no further log output, injected input produces none, every thread parked in a
wait, no `ErrorLog` — the signature this project's notes give for a wild jump rather than a fault.

| arm | runs | result |
|---|---|---|
| reclaim armed (default) | 2 — one on the DLL that adds the generation, one on the build before it | **freeze**, both |
| `tagpu_reclaim.off` | 2 | reaches `MAINMENU.GUI`, log keeps growing, UI still readable |

So it is **this module's teardown wrap**, and it is **not the level generation's doing** — it
reproduces on the build immediately before that change. How far back it goes is **not
established**: `roadmap.md`'s own G14h entry records "an in-process level exit … and a second
game" working on 2026-09-06, so either that run took a different route out of the level or
something has changed since. A clue, not a cause: the surviving runs log a **second**
`tagpu: GL CONTEXT CHANGED` at the menu transition (an in-process map change replaces the
context — `renderers.md`), and the frozen runs never reach it.

**Why the harness never saw it:** every scripted session ends with `tacli stop`, which kills the
process. Nothing in it had quit a level to the menu. It is on by default, so a player who
surrenders a game hits it.

`[2026-09-09] IT DID NOT REPRODUCE.` Seven teardowns on the branch tip (`0d876b9` plus the
`model_root` fix), every one clean — the cascade returned, the post hook ran, the second
`GL CONTEXT CHANGED` was logged and the shell came back:

| run | arm set | at the teardown | result |
|---|---|---|---|
| 1 | native/terr/feat/gui, `one-unit` | queue empty (`flushed 0`) | `MAINMENU.GUI`, clean |
| 2 | same, `cob-tank` (4 commanders cleared) | **`flushed 1`, reader idle** — §6b's own line | clean |
| 3 | + `posebake.on=log check`, `pose-inventory` (69 units) | `flushed 1`, reader idle | clean, `native: level 0 -> 1 … aabb=0 selbox=0 pmap=1` |
| 4 | same, second level in the same process | `flushed 0` | clean |
| 5 | same, `200v200` quit mid-battle (400 units) | `flushed 2` | clean |
| 6 | **play defaults** (`--defaults`: classicpp, shadows, gui, weapons), `200v200` at 7.5 min | `flushed 1` | clean, `aabb=4 selbox=0 pmap=1` |
| 7 | play defaults, second level in the same process | `flushed 0` | clean, caches **repopulated** |

So the reproduction above is not a recipe that works today, and the freeze is **not** simply "the
teardown wrap is armed": every one of those runs had it armed and on by default. What is not
established is why — whether the two frozen runs needed a condition none of these seven had, or
whether something between them and the branch tip changed it. Instrumented breadcrumbs either
side of the cascade (pre enter / pre leave / post enter / post leave) were in the DLL for runs
1-5 and every one of the four lines appeared in order.

**The consequence for the work that was blocked on it: it is no longer blocked.** Runs 3, 4, 6
and 7 are the level-generation invalidation observed end to end — the three `tagpu_native.c`
caches dropped at the teardown and refilled from the next level's templates, which §6a and the
G16 step 3/4 landings could only assert from the code. Run 7's `shadow: caster model=34 top=40.0
(aabb y -1.5..40.0)` is a second level's AABB, rebuilt after the drop.

**Not root-caused,** and one obvious candidate is already **disproven**: the post hook *does* run
on both exits, so `s_teardown` and `s_defer` are restored. `[BINARY-VERIFIED 2026-09-08]` the
tail-jump at `0x491C54` goes to `0x450DD0`, which takes no stack argument and has a single `ret`
at `0x450E19`, so the stub's `call` returns and the post hook is reached — as
`exe-reverse-engineering.md` §`0x491B60` already said. What is left to test: whether the pre
hook's flush plus the `s_defer = 0` synchronous cascade can free one object twice, and what the
render thread is doing across the context replacement the surviving runs reach and the frozen
ones do not.

### 6c. The template frees are deferred too — the timeout stops being load-bearing

`[BUILT 2026-09-09]` §6a left one thing to timing: the pre hook waits up to a second for the
reader, and **on the timeout it lets the cascade run anyway**, so `0x42DB90` could return the
templates to the heap with a render pass still walking them. The level generation does not help
there — it invalidates a *cache* after the fact; it does not keep the memory alive while a walk
is in flight. Nothing downstream could close it either: a reader cannot make a pointer valid by
checking it, and `tagpu_native.c`'s bounded `model_root` guarantees the *slot*, not the block.

So the frees are deferred, with the pattern this page is about, applied to its second client:

- **The two call sites are redirected, not the function.** `0x42DB90` is left to run: its
  `call MEM_Free` at `0x42DC01` (one model's block — the tree, the rest vertices and the faces
  are one allocation) and at `0x42DCB6` (the pointer table) are rewritten to call
  `reclaim_template_free`, which pushes the pointer onto the same ring. Each site is byte-checked
  first — `E8` with a rel32 that really resolves to `0x4D85A0` — and both are landed or neither.
  The other two `MEM_Free`s in that body (`0x42DC23`, `0x42DC52`) are unit-def fields no pass of
  ours reads, and are left alone.
- **The rel32 is computed against the call site, never against the buffer it is built in.** That
  is the wild-call footgun this project has already paid for once.
- **The normal release is not the epoch at all.** When the pre hook returned with the reader idle
  it had already published `s_teardown`, so every `pass_begin` since has quiesced immediately and
  `tagpu_overlay_draw` has returned before its first engine read. No pass in that interval can be
  holding a template, and none can start holding one. The post hook therefore frees the whole
  queue outright, on that guarantee rather than on any elapsed time, and the heap has the level's
  models back before the next one loads.
- **The timed-out path falls back to the stamp.** If the reader was still in its pass, nothing is
  freed at the post hook; those blocks stay on the ring and leave through the ordinary epoch
  drain at the next level's first death — later, never sooner, and leaked outright if the reader
  never runs again. The one-second wait is now a *scheduling* choice: it decides when the memory
  comes back, not whether the read was safe.
- **The generation is still needed and is not made redundant.** It answers a different question:
  the blocks really are freed at the post hook, so the next level's allocator may hand the same
  address to a different model, and a cache keyed on a template pointer must still drop. Deferral
  keeps the walk safe; the generation keeps the cache honest.

`[MEASURED 2026-09-09]` two full level cycles in one process, `200v200` under the play defaults:

```
reclaim: level teardown (gen 0 -> 1): flushed 1 queued object(s), reader idle; the cascade frees synchronously
reclaim: teardown post: freed 279 block(s) the cascade queued (279 model template(s) this session), reader quiesced throughout
native: level 0 -> 1, dropping the template caches: aabb=4 selbox=0 pmap=1
```

then the same again for `gen 1 -> 2` (558 cumulative), with the second level's shadow AABBs
rebuilt in between. The periodic line grows `tmpl=<queued>/<freed by the epoch>/<leaked>`, which
read `279/0/0` and `558/0/0`: every teardown took the quiesced path, so nothing needed the
fallback and nothing was lost. `ovf=0`, ring high-water 279 against 4096.

## 7. The reusable module

**As built (G14h, `tagpu_reclaim.c`; the templates added 2026-09-09, §6c).** Two classes now, one
ring: `RC_OBJ3DO` entries go back through the real `FreeObjectState`, `RC_BLOCK` entries — a model
template's single block and the pointer table — through the engine's raw `MEM_Free`, and
`entry_free()` is the one place that knows which. They share the ring, the epoch, the high-water
mark and the overflow-means-leak policy because they share the hazard; what differs is when they
are normally released (an `Object3do` waits for the epoch because units die while the reader runs;
a template does not need to, because the teardown wrap has already made the reader quiescent).
The drain runs on the game thread **inside the destructor detour itself** (every call first stamps and frees what became safe, then queues its
own object), which needs no tick hook — the engine's tick detour at `0x4969D2` is installed only
while a scenario is being applied, so it could not host the drain. The reader's bracket is one
unconditional `pass_begin` / `pass_end` pair around `tagpu_overlay_draw` in `render_ogl.c`, which
also puts the debug probes inside it; while a teardown is in progress the driver skips only its
engine-reading half, so input injection and the GL-context detection keep running. Level teardown
`0x491B60` is wrapped (pre: fenced flag, wait ≤ 1 s for the reader to leave its pass, then flush and
let the cascade free synchronously — or, if the reader is still busy, free **nothing**: keep the queue
and keep deferring through the cascade; post: release). It is on by default, `tagpu_reclaim.off`
disables, and it logs `reclaim: def=… drn=… queued=… hw=… ovf=… foreign=… flushed=… held=…
teardowns=… tmpl=<queued>/<freed by the epoch>/<leaked> pass=…` every 300 frames (ring 4096), plus
one `reclaim: teardown post: freed N block(s) …` line per teardown. The composite frames are **not** freed by
the destructor (`0x437C90` only unregisters a slot — see §3), so they are not covered and not
needed for the crash.

The second client did not need the generalisation below: its frees are two `call MEM_Free` sites
rather than a destructor with a prologue, so they are redirected in place and the "class" is one
field on the ring entry. The sketch stands for a client that really is a destructor — register it
by address, one ring tagged by class, a shared bracket:

```c
int  tagpu_reclaim_arm(const char* on_file, unsigned dtor_va,
                       const unsigned char* stolen, int nst,
                       unsigned char retn, const char* name);
void tagpu_reclaim_pass_begin(unsigned frame_counter);   /* render: top of pass  */
void tagpu_reclaim_pass_end(void);                        /* render: after emits  */
void tagpu_reclaim_drain_gamethread(void);                /* game: from the tick  */
void tagpu_reclaim_flush(unsigned frame_counter);         /* overlay flush list   */
void tagpu_reclaim_teardown(void);                        /* DLL detach           */
```

It gates on a live on-file (a plain exe arms nothing), byte-matches the prologue and installs
all-or-nothing, mirrors the existing modules' 90-frame silence detector and 300-frame counter line,
and falls back safely if the ring ever fills (leak the overflow, never sync-free — that reopens the
race — and never spin — the drain runs once per pass and the render thread can be stopped). One
detail: the existing `tagpu_detour_leaf_call` is not quite enough — the enqueue stub must pass two
values (the pointer and which object class it is) and expose the trampoline so the drain can call the
real free, so a small new detour builder is needed. Registering a new object type later is one `arm`
call plus, if the object is read by a pass not already bracketed, the three pass hooks.

**Integration checklist for a new object type.** (1) Find its destructor and confirm every death
routes through it. (2) Record the calling convention, the prologue steal (≥5 bytes ending on an
instruction boundary), the resume VA, and the `ret n`. (3) Confirm the destructor is safe to run on
the drain thread, or plan the game-thread drain. (4) One `tagpu_reclaim_arm` in `DllMain` beside the
other own-draw inits, plus the on-file in the gamedir. (5) Bracket the reader's pass if not already.
(6) Verify the byte-match logs armed, then watch the counters. (7) Document the destructor, its
callers and the free-before-invalidate ordering in **Reverse-engineering the exe**; the hook and gate
in **GPU status, hooks & limits**; a roadmap row; regenerate the wiki.

## 8. Why this is sim-safe, and how to prove it

The pattern changes only when render-cache memory returns to the heap. It does not change any value
the simulation reads: the engine still nulls its pointer and clears the alive bit, and nothing reads
the model object or posed geometry back into sim state (see **Frame composition** §4). Two stock
multiplayer peers already run with different heap layouts and stay in lockstep, which proves the
simulation cannot depend on allocation addresses or order (see **More weapons per unit** on the CRC
lockstep model). The determinism risk is therefore low, but the failure mode of this codebase is
silent, so it must clear these gates before landing:

- **Crash gone.** At least five consecutive clean runs of the `200v200` scenario (the crash hits
  about two runs in three, so five clean runs put a fluke under half a percent), each lengthened to
  about five minutes, with the vectored-handler probe logging zero access violations inside our DLL.
- **Counters healthy.** Deferred count rising in combat, drained tracking deferred over time, zero
  fall-backs, ring high-water far below capacity, queue returning toward zero in lulls.
- **Classic parity.** The engine's own 8-bit surface byte-identical to a stock DLL with the pattern
  armed — it must move no pixel.
- **Determinism.** A roster A/B diff (position, health, kills, reload, targets at fixed ticks) armed
  versus unarmed over a full fight and an AI skirmish, and the G9 replay byte-diff when an
  interactive session is available. Both must be identical.

## 9. Invariants the implementation must hold

These are the correctness and liveness conditions the mechanism rests on. Each is cheap to keep
and silent to break, so the landing must check every one.

- **The bracket closes on every exit path.** Every path that publishes pass-started must publish
  pass-completed, including the six early `return`s in `tagpu_native_frame` and the
  `tagpu_overlay.off` path. A pass that starts and never completes leaves the reader permanently
  "busy": the drain's `completed ≥ snapshot` test never passes and **all reclamation halts** for the
  rest of the session — a leak of every death, not a crash. Put pass-completed in the *caller*
  (the overlay driver, after the pass returns) so one unconditional statement covers all exits.
- **Every read of a registered class sits inside the bracket.** The proof covers only reads between
  pass-started and pass-completed. Today the debug probes read the model object *outside* it:
  `probe_unit_model` (`tagpu_overlay.c:231`, runs before the native pass) and `pose_dump`. Move
  them inside the bracket or leave them disabled; do not add new out-of-bracket reads.
- **Single reader.** The two counters assume one reader thread. If a second reader ever
  dereferences a registered class (a worker thread, a second render pass on another thread), each
  reader needs its own pair and the drain must wait on all of them.
- **Wrap-safe comparisons.** The counters are 32-bit and monotone; compare with
  `(int)(completed − snapshot) >= 0`, never with `<`/`>=` on the raw values, or the first wrap
  frees everything early.
- **Snapshot after unreachability.** The snapshot of pass-started must be taken after the object is
  unreachable to new gathers (after the engine's null). The game-thread drain gives this for free by
  program order; a render-thread drain does not.
- **No address reuse while held.** Because the block is not returned to the heap until quiescence,
  the allocator cannot hand its address to a new object while a reader may still hold it, so the
  ABA hazard is closed by construction — provided *every* free of the class goes through the
  detour (it does: the three callers all reach `0x45AAA0`).

## 10. Open items to settle during the landing

- Disassemble the composite-detach helper `0x437c90` to confirm it is what forces the game-thread
  drain, then adopt the game-thread drain so the point is moot.
- Reclaim by **quiescence**, never by a fixed pass count: the reader publishes pass-started and
  pass-completed counters; the game-thread drain snapshots pass-started *after* the object is
  unreachable and frees only once pass-completed has passed the snapshot. This removes the
  free-to-null bounded-preemption assumption entirely (§4).
- Size the ring well above the worst death rate (a thousand slots is two orders of headroom) and
  make the full-ring fallback a leak, never a sync-free and never a spin.
- Fold in the two particle heap surfaces as a second Mode A client once the model-object fix is
  proven; handle the fog grid separately with a per-frame snapshot.
- Store the wreck record in the gather record so the cheap pointer re-read (the belt-and-suspenders
  companion) works for wrecks as well as units.
- **Classify the composite frame's lifetime.** The render thread reads `obj+0x10` and then the
  frame header at `tagpu_native.c:1786`, but `FreeObjectState` only unregisters that slot; the frame
  belongs to the composite draw context at `*(TA+0x1437B)`. Establish whether that owner frees or
  recycles frames (Mode A needs its own client; Mode B needs nothing) — see **Composite buffer (G6)**
  for the frame format and the owner's blit path.
- ~~Move `probe_unit_model` and `pose_dump` inside the bracket~~ — done: the bracket wraps the
  whole overlay driver in `render_ogl.c`, so every render-thread read is inside it.
- ~~Publish pass-completed from the overlay driver~~ — done: one unconditional pair in
  `render_ogl.c` around `tagpu_overlay_draw`.
- **The drain is pumped only by frees** (no game-thread tick hook exists outside a scenario apply),
  so the most recent death's object and its composite-registry slot are held until the next death or
  the level ends. Accepted for one object per lull — the engine draws from the unit array, not the
  registry — and flagged by the review; a periodic game-thread drain point would close it.
- ~~Classify the composite frame's lifetime before landing~~ — **still open**, but not needed for
  the crash: the emit reads `obj+0x10` only for the waterline depth-plane test, and the frame's
  owner is the composite draw context (`main+0x1437B`, nulled at teardown `0x42DCA3`).

## 11. Bottom line

Classify each render-read object by how the engine reclaims it; for heap-freed objects, detour the
destructor to defer the free behind a render-pass grace period and drain on the game thread; for
pooled objects, accept the torn read. The model object is the one Mode A client that fixes the crash
today, reached through the single function `FreeObjectState`; the particle sub-vectors and layer
arrays are the next Mode A clients; everything else is already safe. The pattern is race-free,
renders the correct final frame rather than garbage, needs no hot-path lock, and is transparent to
the simulation.

## Changelog

- **2026-09-09** — **second client landed: the model templates** (§6c). `0x42DB90`'s two
  `MEM_Free` call sites are redirected onto the same ring, so the templates outlive any render
  pass that is walking them and the pre hook's one-second timeout stops being a safety argument —
  it now only decides *when* the memory comes back. Measured over two level cycles: 279 blocks a
  teardown, `tmpl=279/0/0` then `558/0/0`, `ovf=0`, nothing lost. §6b gains the freeze's
  non-reproduction table (seven teardowns, three arm sets, all clean), and §2 gains what a Mode B
  read owes — the bound that an unbounded `ModelId` was missing when it crashed the shadow pass.

- **2026-09-06** — first draft. Design synthesised from six investigations; not yet landed. Crash
  root cause is recorded in **GPU status, hooks & limits** §3.2.
- **2026-09-06** — corrected the grace argument (§4, §9). Reclamation is **quiescence-based**: free
  only once the reader has published completion of the passes that could hold the object. The
  earlier "free N passes behind" wording was a probabilistic margin, not a proof — it assumed the
  engine's null landed within N−1 reader passes, which a preempted game thread can violate.
- **2026-09-06** — **reviewed** (two Opus reviewers at high, ten findings, eight acted on). The
  serious one: on the teardown wait's timeout the code leaked the queue and then switched
  deferral *off* for the cascade — hundreds of synchronous frees (`0x485980` frees every unit
  through the death routine, which this page's first draft denied) under a reader that might
  still be in its pass. Now the queue is kept and deferral stays on. Also: the teardown routine has
  two exits (a tail-jump to `0x450DD0`, itself argument-free); the refused pass no longer skips the
  whole overlay driver; the dead-unit skip ran before the depth key was stored; the owner thread is
  the fork's recorded game thread, not the first caller.
- **2026-09-06** — **built and measured** as G14h (`tagpu_reclaim.c`): drain inside the destructor
  detour on the game thread (no tick hook available), teardown wrapped, bracket around the whole
  overlay driver, on by default with `tagpu_reclaim.off`. First `200v200` fight: `ARMED` on both
  sites, 147 deferred / 145 drained at the 2700-frame mark, high-water 3, overflow 0, no foreign
  thread, no fault.
- **2026-09-06** — audit for missing elements. Added §9 *Invariants* (bracket closes on every exit
  or reclamation halts; every read inside the bracket, which the debug probes currently violate;
  single reader; wrap-safe compares; snapshot after unreachability; ABA closed by construction).
  Fixed the free test to `completed ≥ snapshot` (a strict `>` leaks every death while idle).
  **Corrected a factual error:** `FreeObjectState` does not free the composite frames — `0x437C90`
  zeroes a registry entry and makes no call — so the frame at `obj+0x10` has a separate owner and
  its lifetime is now an open item, not "covered for free".
- **2026-09-08** — `tagpu_reclaim_level_gen()` added (§6a): the model templates are a lifetime the
  destructor detour never covered, and `tagpu_native.c`'s three pointer-keyed caches now drop on
  it. Verified on a real teardown. In the course of that, found §6b: **the teardown wrap freezes
  the game on quit-to-menu**, reproducible 2/2 against 2/2 clean with `tagpu_reclaim.off` — not
  the generation's doing, but how far back it goes is unestablished; open, not root-caused.
