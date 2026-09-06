# Thread-safe destruction — deferring engine frees the render thread still reads

*An evolving reference for one recurring hazard: the fork's async GL render thread dereferences an
engine object that the game thread frees underneath it. This page records the confirmed crash, the
reusable pattern that closes it, the object catalogue it applies to, and the gates it must pass
before landing. It is a **design**, not yet in the tree — update it as the pattern is built and
measured. Engine addresses are* <span class="pill pill-ok">VERIFIED</span> *from `TotalA.exe`
(ImageBase `0x400000`) unless marked* <span class="pill pill-warn">INFERRED</span>*. First written
2026-09-06 from six parallel investigations; see also **GPU status, hooks & limits** §3.2 (the crash
as a known limit), **Memory Manager Investigation** (the heap this reclaims into), **Own the draw**
(the detour discipline reused here), and **Effects** / **Features** (the other objects the render
thread reads).*

## Status

| | |
|---|---|
| **Problem** | <span class="pill pill-ok">CONFIRMED</span> cross-thread use-after-free, `200v200` ~95 s in, ~2 of 3 runs |
| **Pattern** | designed, **not landed** — a separate task, from a fresh worktree, reviewed on Opus at `high` (sim-adjacent) |
| **First client** | the model object `Object3do` (units + wrecks + features), via `FreeObjectState 0x45AAA0` |
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
  at worst a one-frame cosmetic glitch, never a crash. These need nothing beyond the field
  validation the fork already does (`ptr_ok` range test plus `IsBadReadPtr`).

That single rule tells you, per object type, whether it needs the pattern.

## 3. What actually needs the pattern (the catalogue)

| Object | Class | Destroyer | Status |
|---|---|---|---|
| **Model object (`Object3do`) — units + wrecks + features** | A | `FreeObjectState 0x45AAA0` (callers `0x486D9E` unit, `0x42474F` wreck, `0x4221C4` bulk) | the confirmed crash — fix first |
| Its posed vertex buffers | A | freed *inside* `FreeObjectState` (per-prim loop at `0x45AAB2`) | covered for free by deferring `FreeObjectState` |
| Its composite frame (`obj+0x10`, read at `tagpu_native.c:1786`) | **unclassified** | **not** freed by `FreeObjectState` — `0x437C90` only zeroes a registry entry and makes no call; the frame's owner is the composite draw context at `*(TA+0x1437B)` <span class="pill pill-warn">INFERRED</span> | separate lifetime — classify Mode A/B before landing (§10) |
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

## 7. The reusable module

Package it as one small module (`tagpu_reclaim` <span class="pill pill-warn">INFERRED name</span>)
beside the existing detour helper, so any own-the-draw module makes an object type safe by
registering its destructor once:

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
- Move `probe_unit_model` and `pose_dump` inside the bracket, or keep them disabled (§9).
- Publish pass-completed from the overlay driver, not from inside the pass, so every early return
  closes the bracket (§9).

## 11. Bottom line

Classify each render-read object by how the engine reclaims it; for heap-freed objects, detour the
destructor to defer the free behind a render-pass grace period and drain on the game thread; for
pooled objects, accept the torn read. The model object is the one Mode A client that fixes the crash
today, reached through the single function `FreeObjectState`; the particle sub-vectors and layer
arrays are the next Mode A clients; everything else is already safe. The pattern is race-free,
renders the correct final frame rather than garbage, needs no hot-path lock, and is transparent to
the simulation.

## Changelog

- **2026-09-06** — first draft. Design synthesised from six investigations; not yet landed. Crash
  root cause is recorded in **GPU status, hooks & limits** §3.2.
- **2026-09-06** — corrected the grace argument (§4, §9). Reclamation is **quiescence-based**: free
  only once the reader has published completion of the passes that could hold the object. The
  earlier "free N passes behind" wording was a probabilistic margin, not a proof — it assumed the
  engine's null landed within N−1 reader passes, which a preempted game thread can violate.
- **2026-09-06** — audit for missing elements. Added §9 *Invariants* (bracket closes on every exit
  or reclamation halts; every read inside the bracket, which the debug probes currently violate;
  single reader; wrap-safe compares; snapshot after unreachability; ABA closed by construction).
  Fixed the free test to `completed ≥ snapshot` (a strict `>` leaks every death while idle).
  **Corrected a factual error:** `FreeObjectState` does not free the composite frames — `0x437C90`
  zeroes a registry entry and makes no call — so the frame at `obj+0x10` has a separate owner and
  its lifetime is now an open item, not "covered for free".
