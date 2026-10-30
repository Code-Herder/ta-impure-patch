# Deferred reclamation of the engine's model objects

tagpu_reclaim.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.7 Deferred reclamation of the engine's model objects (`tagpu_reclaim.c`, on by default, `tagpu_reclaim.off`)

The one module that patches nothing the engine *draws* with: it changes **when** a freed block
goes back to the heap, and nothing else. The render thread gathers a unit's or wreck's
`Object3do` and dereferences it later in the same frame; the game thread frees it on death —
`200v200` faulted about 95 s in, two runs in three (`0x486D9E`: the object is freed one
instruction *before* its pointer is nulled and well before the alive bit is cleared, so the
gather's alive gate cannot help). Two sites, byte-matched, all-or-nothing, installed at
`DllMain`, disjoint from every detour above:

| site | mechanism | what |
|---|---|---|
| `FreeObjectState 0x45AAA0` | prologue detour, `tagpu_detour_leaf_call` shape built by hand so the stolen tail is also a callable trampoline | while armed the call **enqueues** the object and returns (`ret 4`); the engine's own null of `unit+0x9E` / `rec+4` and its alive-bit clear run unchanged. Every `Object3do` free — unit death, wreck destroy, the bulk teardown loop — goes through this one entry |
| the model templates' two frees, **`0x42DC01`** and **`0x42DCB6`** (inside `0x42DB90`, whose one caller is the teardown cascade) | **call-site redirect**: each 5-byte `call MEM_Free` rewritten to call ours, after checking the site really is `E8` with a rel32 resolving to `0x4D85A0`; both or neither, and the rel32 is computed against the call site rather than the buffer it is built in | a template block — one allocation carrying the whole tree, its rest vertices and its faces — and the pointer table are **enqueued** on the same ring instead of freed. Released by the post hook below whenever the pre hook proved the reader idle, which is the guarantee rather than any elapsed time; on the timed-out path they fall back to the epoch stamp. This is what stops the pre hook's timeout from being a safety argument — [thread-safe destruction](../thread-safe-destruction.html) §6c. **Five `MEM_Free`s live in that body, not four**: `0x42DC23` and `0x42DC52` are unit-def fields no pass of ours reads, and `0x42DCCB` frees the whole UnitDef array at `main+0x1439B` — which we *do* read (`tagpu_order`, `tagpu_cat`, `tagpu_weapons`, `tagpu_scenario`), safely only because every reader and this cascade are on the game thread |
| level teardown `0x491B60` | **wrap**: `pushad; call pre; popad; call <stolen tail>; pushad; call post; popad; ret` (no stack args; two exits — `ret` at `0x491C59` and a tail-jump to `0x450DD0`, which also takes nothing and returns with `ret` — so its body can be called and returns to us either way) | pre: raise a flag (fenced), wait ≤ 1 s for the render thread to leave its pass; if it does, **flush** the queue through the real destructor while the composite registry it walks is still alive and let the cascade (every unit, through `0x485980 → 0x4864B0 → 0x4866D0`, plus the wreck loop) free synchronously; if it does not, **nothing is freed** — the queue is kept and deferral stays on through the cascade (`held=`), draining at the next game's first deaths; post: free every block the cascade queued if the reader was proved idle (one `reclaim: teardown post: freed N block(s) …` line), bump the level generation, deferral back on, flag down |

**The reclamation rule is quiescence, never a count.** `render_ogl.c` brackets
`tagpu_overlay_draw` — the render thread's only reader of these objects — with
`tagpu_reclaim_pass_begin` / `pass_end`, one unconditional pair so every early return inside the
overlay closes it. The reader publishes *pass-started* (an `InterlockedIncrement` before its first
engine read) and *pass-completed* (after its last). The real free runs on the **game thread**, from
the next `FreeObjectState` call: every entry already queued has had its record nulled since
(program order), so after a full fence the drain stamps them with pass-started and frees each
once pass-completed has reached its stamp. An idle reader passes at once; a reader mid-pass makes
the entry wait for that pass; a stuck reader freezes reclamation and the 16 384-entry ring leaks
on overflow — never a synchronous free, never a spin. The drain is pumped only by frees, so the
most recent death's object (and its composite-registry slot) is held until the next death or the
level ends — one object for the length of a lull, harmless: the engine draws from the unit array.
While a teardown is in progress the driver skips only its engine-reading half (input injection,
the GL-context-change detection and the flushes still run). Measured on `200v200`: high-water 6,
overflow 0, every free drained within a frame or two. The engine sees no different value: the
sim never reads the object or its posed geometry back, and two stock peers already stay in
lockstep with different heap layouts. `tagpu_native.c` keeps a belt-and-braces re-read of the
record pointer before each emit (wrecks included: the record is kept in the gather now) and skips
a unit whose object moved (`reread=N` on the `native:` line).

Read it in `tagpu.log`: `reclaim: ARMED FreeObjectState@0x45AAA0 -> deferred …` at launch, then
every 300 frames `reclaim: def=… drn=… queued=… hw=… ovf=… foreign=… flushed=… held=…
teardowns=… pass=…` (`def` deferred, `drn` drained, `ovf` must stay 0, `foreign` a call from a
thread other than the game thread — freed synchronously and counted, never seen; `held` the
teardowns where the reader did not leave its pass in time), and at a level change either
`reclaim: level teardown: flushed N queued object(s), reader idle; the cascade frees synchronously`
or `… reader still in its pass after 1000 ms — N queued object(s) KEPT, the cascade's frees deferred`.
`tagpu_reclaim.off` in the gamedir disables the whole module at launch (the A/B lever, and the
way back to the racing build). Design, proof and the object catalogue:
[Thread-safe destruction](../thread-safe-destruction.html).
