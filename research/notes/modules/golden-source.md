# The golden source

The capture of the engine's source on the thread that draws it.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.82 The golden source moves to the thread that draws it — and the old site's tear, measured

**The clean cut (§2.81) left one correctness bug in its own instrument, and named it: the
reference capture ran on the RENDER thread with nothing sequencing it against the game thread's
draw.** That is what this landing fixes. The argument for the old site was `g_ddraw.cs`, and that
section is a LIFETIME argument for the POINTER — it serialises `dds_Flip`'s swap and
`dds_SetPalette` — never a bound on the BYTES: `dds_Lock` takes `This->cs` only under
`g_config.lock_surfaces` and never `g_ddraw.cs` (`ddsurface.c:1014`), so TA rasterises into
`primary->surface`, and `FlipOffscreenToPrimary 0x4C63A0` copies the engine's offscreen onto it,
entirely outside it.

#### The tear was real, and here is the number

**223 of 16 500 reads at the old site came back torn — 1.35 %, 198 704 bytes in all.** Measured
2026-09-20 with a temporary probe (removed before the landing) that read the primary twice from
`tagpu_surf_sync`, i.e. exactly where the copy used to run, holding `g_ddraw.cs` exactly as the
old code held it, on `feat-forest` at 1024×768 with two units patrolling. The same double read at
the NEW site answered **0 bytes of 786 432 on every one of 14 checks**, across four runs, with the
scene in motion.

**A 20-sample version of that probe found nothing, and reporting it would have been wrong.** The
engine is inside the flip for a few per cent of wall time and a probe's window is a few hundred
microseconds, so twenty spaced samples expect well under one hit. Absence of an observation is not
absence of a race — the continuous probe is what turned "the section is not a bound" from an
argument into a measurement. [The first run of this landing's verification did exactly that and
had to be thrown away.]

#### Where the copy runs now

`tagpu_packet_pub.c`'s `after_draw` observer on `DrawGameScreen 0x468CF0`, on the in-play draw
only, after the packet is published. The flip is called from inside that function at `0x46A3DB`,
34 bytes before its `ret` at `0x46A3FD`, so at the observer's `after` the engine has finished
writing the primary — and the thread that writes it is the one running the copy. No lock is needed
for the bytes and none is claimed; `g_ddraw.cs` is still taken, now as the weaker of two arguments
and for uniformity with every other reader of that object.

**It also pairs the reference with the packet — at the producer, which is not the same as at the
consumer.** The same call publishes the state our passes render from, so the two are taken from
one engine frame, and that is the reason this hook was chosen over the flip's own entry (which
would give the previous frame: complete, but one behind).

**An earlier draft of this section said they therefore ARRIVE as one frame, and that is wrong**
[the landing review, confirmed against the code]. They travel on two independent gates — the
packet's `PKX_FRESH` and this module's `req`/`ack` — so a draw can publish without capturing and
capture without publishing. The render thread also acquires the packet at the TOP of
`render_vk.c`'s frame and calls `tagpu_surf_sync` after, inside `tagpu_overlay_draw`, so the
frame it draws can render packet D-1 against reference D. **`TAGPU_SURFFRAME.stamp` carries the
in-play draw number the snapshot was taken at, and it is how a comparison checks the pairing.
Nothing gates on it** — a consumer that needs the two to match must compare `stamp` against
`f->packet`'s own draw number and skip the frame itself.

**And it closes a second hole nobody had named.** The palette used to be read BEFORE the critical
section, from `tagpu_pal.c` — a module whose every entry point is render-thread only, which the
old call violated outright. It is now read from the primary's own palette object inside the same
section as the bytes, so the indices and the table they resolve through cannot be a frame apart.

#### The hand-over: two buffers and one ownership rule

Moving the copy would only move the race unless the reader is ordered too. `tagpu_surf.h` carries
the rule; the shape is: `hold` names the buffer the render thread owns, the game thread writes
only `1 - hold`, `req`/`ack` counters make a capture *in flight* exactly while they differ, the
render thread moves `hold` only when nothing is in flight, and each side's publish is a RELEASE
store the other ACQUIREs. The two threads therefore never touch one buffer, and `hold` never moves
under a capture. The game thread may READ the held buffer to compare bytes against the last
snapshot — two readers is not a hazard.

**That is also what bounds the cost.** A capture needs a request and a request needs a sync, so the
game thread copies at most once per render frame and a render thread that is behind costs it
nothing: `norequest=` counts the in-play draws that paid one acquire load and a compare.

**The reference's lifetime is the level's, and the level is a property of the SNAPSHOT.**
`tagpu_surf_level_end` rides `tagpu_packet_pub_level_end` and bumps one counter; each capture is
stamped with the counter it was taken under; and both the render thread's adopt and
`tagpu_surf_frame`'s serve refuse a snapshot whose stamp is not the current one. So the reference
cannot outlive the level it was taken from and be diffed against the next one. `surf: the level
ended - the golden source is dropped` says so in the log.

**Both halves of that were wrong in the first version of this landing, and the landing review
found them.** The first was a flag the producer cleared for the drop and then, four lines later
in the same call, re-set by adopting the dead level's final capture — the *common* path, because
a teardown usually lands with nothing in flight. The second is the one that matters here: the
producer's half runs inside `tagpu_overlay_draw`, **below its `tagpu_reclaim_teardown_active()`
early return** — which is a level teardown, the precise event the drop exists for — while the
consumer `tagpu_vk_surf_prepare` is called from `tagpu_vk.c`'s frame record and shares neither of
those returns. A flag in the producer could therefore never be cleared on the frames that needed
it. Making the level a stamp on the data fixes both at once, and puts the test on the path every
reader takes.

#### What it measured

`feat-forest`, Two Continents, 1024×768, **the play defaults**, `renderer=vulkan`, census
`5 pass(es) drew (terr=1 feat=1 unit=1 fx=1 mark=1)`.

| | |
|---|---|
| the invariant, SPOT-CHECKED | **0 byte(s) of 786 432** differ between two reads of the primary at the capture site — 14 of 14 one-shot checks, four runs, with two units patrolling. **This row is not a measurement, and the first version of this section presented it as one** — see the review table below |
| the old site, the continuous probe | **223 of 16 500 reads TORN, 198 704 bytes** |
| the golden source against an independent read | **0 differing px of 786 432** against `tacli shot`, which reads the same primary from the other game-thread hook (`before_flip`, i.e. the previous completed frame), on a paused settled scene |
| the picture | 190 distinct colours in the viewport, 92 in the sidebar, **0 raw key, 0 teal** — a complete 1997 frame |
| cost on the game thread | **55–58 µs mean, 136–289 µs max** per capture, at 1024×768 — the row loop, the comparison against the previous snapshot and the palette together |
| the gate | `captured=1800 unchanged=1576 refused=0 norequest=2944` — two thirds of in-play draws pay a load and a compare and nothing else |

#### And what the REVIEW FIXES measured, the same day

The four corrections above (the level stamp, the reachable drop, the foreign-thread level end, the
two documentation overclaims) were re-verified by running, on `feat-forest` / Two Continents /
1024×768 / `renderer=vulkan`, with `tagpu_defaults.off` and `terr feat fx own+` armed explicitly —
**not** the play defaults, which is why this instance's golden source is 49 % raw key where the
table above reads 0: the `*own` levers skip the engine's own draw, and the key that leaves in the
reference is §2.81's known hole, not a regression.

| | |
|---|---|
| the invariant, again (one-shot) | **0 byte(s) of 786 432**, 10 of 10 checks across two levels — six with seven units patrolling, four after a level cycle. Still a spot check; the row below is the measurement |
| **the invariant, MEASURED — the same probe the old site was condemned by** | **0 of 21 669 checks torn, 0 bytes, worst 0**, continuous: 18 268 in the first level with seven units patrolling, then 3 401 more after a full teardown and a fresh level. Against the old site's **223 of 16 500**. Zero events in 21 669 puts the 95 % upper bound on the rate near **0.014 %**, so the two sites are separated by at least ninety-fold rather than by a sample too small to tell them apart |
| **captures discarded because a teardown landed mid-copy** | `straddle=0`, over 21 000 captures and a real level cycle — the new counter, and also the instrument for the foreign-thread question under *Still open* |
| what the probe itself costs | `us avg=373 max=535` armed, against `43–59` unarmed: it re-reads and compares the whole surface on every capture and holds `g_ddraw.cs` across it. A lever you arm for a run and take away again, never a default |
| **the drop, across a real level teardown** | `surf: the level ended - the golden source is dropped`, and then the consumer **stops dead**: 6 `vk: surf:` heartbeats in the window before the drop, **0 in the 3 172 log lines after it**, through the whole shell |
| **and it comes back** | the second level re-armed from nothing — `vk: surf: the reference texture is up` and the heartbeats resume, `refused=0` throughout. The drop ends the reference; it does not poison it |
| the golden source against an independent read, in motion | **352–529 px of 786 432** (median 397) against `tacli shot` — against a **shot-vs-shot control of 415–508** (median 453) on the same scene, both confined to one 22×55 px region holding an animating smoke plume. The capture is as close to an independent read of the primary as two independent reads are to each other; the 0 in the row above is the same measurement on a scene with nothing animating in it, and needs one |
| cost, unchanged | **52–59 µs mean, 112–362 µs max**, `captured=388500 unchanged=381603 refused=0` |

#### `tagpu_surfdump.on` — the oracle the reference did not have

The golden source has no consumer in the tree, so there was no way to look at it and no way to
check any claim about it. The lever is one shot, self-deleting, and answers both questions:
`surf: re-read check at draw N: 0 byte(s) of M differ` from the game thread, and
`tagpu_surf.ppm` — the snapshot resolved through its own palette — from the render thread. It is
an oracle rather than instrumentation, which is why it stays.

#### Still open, and what this does NOT do

* **There is no golden source in the SHELL, and there cannot be at this hook.** The shell never
  calls `DrawGameScreen` (`tagpu_packet_pub.c`, the cursor channel's "WHY NOT THE FLIP"), and the
  only per-present site it does have — the cursor draw `0x4C67C0` — is *inside* the flip, where
  the primary is mid-write. `tacli shot` is the answer there and is unchanged.
* **On `renderer=gdi` nothing captures**, because `tagpu_surf_sync` runs from
  `tagpu_overlay_draw` and that lane makes no `tagpu_` call. Nothing asks, so nothing is paid for;
  the reference is the Vulkan lane's instrument.
* **`markown` still holes the reference** — while our marker pass draws, the engine's bars,
  digits, order markers and build cursor are skipped, and `tagpu_mark.on=passive` is the arm that
  hands them back (§2.81). Unchanged by this landing and still the one hole the play defaults
  leave.
* **A reference the game thread has not refreshed goes stale rather than absent.** The `stamp`
  field carries the in-play draw it was taken at so a comparison can say what it read; nothing
  gates on it, and the level-end drop is what covers the case that matters.
* **`stamp` is also the only way to check the reference against the packet.** They are published
  in one engine call but delivered through two gates, and the render thread takes the packet
  first — so a frame can draw packet D-1 against reference D (above). A comparison that depends
  on the pairing compares the two numbers itself.
* **SETTLED 2026-09-20: the level teardown is GAME-THREAD ONLY — on the evidence of a
  measurement and a standing instrument, not of the call graph.** The survey is in
  [the level teardown `0x491B60`](../engine/memory.md#who-enters-it-and-on-which-thread), *Who enters it*; the short
  form:
  * `0x491B60`'s **address is never taken anywhere in the image**, and there is no `.reloc`, so
    its entry set is closed: six direct calls and nothing else. That is the part the static work
    does establish.
  * **All six sit in four functions that have no direct caller.** Each is address-taken and
    entered through the engine's state-handler slot `[main+0x391F5]`, dispatched from the single
    site `0x499A1C`. So the call graph stops at a function pointer.
  * **Three of the six are nevertheless pinned by construction.** `0x499200` holds those three
    *and* calls the in-game frame callback `0x496790` directly — once at `0x4996A5`, the
    instruction immediately before the teardown call at `0x4996AA`. `0x496790` is the function
    the publisher gates with `on_game_thread()` on every in-play draw, hundreds of thousands a
    session, never once foreign. Same function, same straight line, same thread.
  * **The other three rest on the measurement alone**, and the asymmetry is unflattering: by
    `0x499200`'s behaviour the teardowns that were measured went through the three sites that
    are *also* pinned statically, so the three carried by measurement are the least exercised.
  * The loader thread's **direct-call** closure (1 208 functions) contains neither the teardown
    nor the dispatcher nor the state-setter — worth having, but it is a direct-call result, and
    that closure also holds 690 indirect call sites.
  * **The identity is measured:** four teardowns over two distinct UI paths, all on the game
    thread, with the loader thread alive on a different id at the same moment; plus 50 teardowns
    across the 299 surviving instance logs with no foreign level-end ever recorded. `tdforeign=`
    and the thread id in the teardown line now re-check it on every launch.
  * **A first version of this bullet claimed three of the six call sites were dead code. They
    are not** — they are in `0x499200`, a live state handler, and by its behaviour it is the
    quit and restart paths, i.e. probably the very sites the measurement exercised. The cause
    was attributing call sites to the nearest preceding call target, which is blind to
    functions that are only ever dispatched through a pointer. The correction and the method
    that caused it are written out in the engine map.
* **The two sentences that looked contradictory were answering different questions.**
  `reclaim_template_free` checks the thread because it *has* a fallback: hand the block to the
  system free. The teardown hooks have none — the pre hook's job is to quiesce the render thread
  and flush the ring, the post hook's is to free what the cascade queued — so an early return on
  a foreign thread would leave the reader unquiesced and the deferral state wrong. **A guard
  there would convert a question about identity into a real fault.** The right answer was to
  measure, which is what `tdforeign=` and the thread id in the teardown line now do on every run,
  including configurations this session cannot reproduce.
* **Two guards in the golden-source path are therefore belt-and-braces, and stay.** The drop
  sitting above `tagpu_packet_pub_level_end`'s thread check is not expected to matter, and the
  straddle discard is not expected to fire — one thread cannot be inside `take_into` and in the
  teardown at once, which is why `straddle=0`. Neither is stated as "can never", because what
  rules it out is a measurement plus a closed entry set, not a proof over the call graph. Both
  are one compare, and the reason to keep them is exactly that they hold the module correct
  **without** depending on the survey staying true.
