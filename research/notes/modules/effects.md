# The effects' models

The engine's rasterisers, transcribed.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.89 The effects' models, drawn — the engine's rasterisers, transcribed

**What was wrong.** No Vulkan pass drew an effect's 3D model: a weapon's body and its thrust
flame (rendertypes 1, 3 and 6), an explosion's body, a debris piece. The native pass wrote their
vertices into an array nothing read, and `fxown` skips the engine's own draw of them
(`0x46BAE0`, `0x4211D0`), so a rocket at the head of its smoke trail was not on screen at all
(§2.88, *Not covered*, measured on `fx-rockets`).

**The survey, and the design chosen.** Three ways to put them back:

- **The engine's own pixels** — not available: the engine's frame reaches no pixel of the
  screen (§2.81), and the 8bpp path is deleted (§2.88).
- **Geometry on the GPU** — the node posed and drawn as two triangles a face with interpolated
  UVs, as the unit bodies are. Built and measured against the engine's own draw, paused at 1×:
  **3 of 20** model pixels differed on a frame of rockets, **10 of 62** on a mixed one and **309
  of 831** on `big-battle`. The engine walks each edge in 16.16 from a vertex biased by `0xFFFF`,
  steps its texture coordinates by truncating divisions and addresses the texel by the frame
  width's own rule, and a rocket at 1× is a few dozen pixels, every one of them decided by that
  arithmetic. Discarded, and its code deleted.
- **The engine's rasterisers, transcribed** — chosen. They are integer code with no state but
  the clip rect, so a transcription reproduces them exactly, and their output is small.

| where | what |
|---|---|
| `tagpu_packet_pub.c` `fx_model` (game thread) | poses each model a frame can show with the engine's own rotation `0x4B6CC0` into a `dst` of its own, adds the effect's 16.16 position, and copies it with its faces and every frame already resolved by the draw's own rule (`0x4B7EE0` for an animated face, `0x4B7F30` with the owner's logo colour for a debris piece's team face) into five tables — model, shape, face, index, vertex (`tagpu_packet.h`, *THE EFFECTS MODELS*). A shape is shared by every model of the gather that draws the same node with the same frames. **A model no frame can show is not posed, and that is tested first**: its node's rest vertices alone give it a margin about its anchor (`2·r + 4` px, `r` the largest sum of a rest vertex's three absolute coordinates, kept per node for the gather by `fx_rad`), and a model whose margin misses the packet's reach (`fog_reach`: the published window at the zoom floor, the lead and the fog grids) carries none (`fx_in_reach`) — before its faces are read, so a face it could not have shown never takes its record back. **A model is whole or not carried**: a face the engine draws nothing for is left out (a NULL frame, a NULL sequence, `0x4B7F30`'s out-of-range answer), one it draws and the packet cannot carry refuses the model (the engine map, *The effects models*), and so does a node with no vertex and a face to paint, whose faces the engine walks over stale points; a node with no face to paint carries no model while its record's other parts draw. Every count and index is bounded before it is used, and every table stops at the acquire's own `TAGPU_PK_TABLE_MAX` rows and the five together at a slot's reserve (`fx_fits`), so no table the publisher lands is one the acquire refuses. A model not carried leaves its record's index past `n_fxmodel` |
| `tagpu_packet_pub.c` `fill_fxmodels` | the five tables, **last** of the fill and together or not at all: a slot too small for them cuts them alone (`TAGPU_PK_TRUNC_FXMODEL`), gives back the cursor they advanced, and asks for all five in the next fill's size — every record with a model is then taken back whole for that one frame |
| `tagpu_packet.c` `frame_valid` | the acquire checks every index of the five tables before the render thread sees them, so the consumer follows them without a check of its own |
| `tagpu_fx.c` `emit_model` | hands each model to the rasteriser as its record is emitted, keeps its runs, its key and its fog anchor, and counts a model it cannot draw as a lost part of its record (the bracket below) |
| `tagpu_fxmodel.c` (render thread) | projects the posed vertices with the effects pass's own eye, `(v >> 16) − eye + 0x80` across and the engine's `y − alt/2` down, and runs `0x46BAE0`'s and `0x4211D0`'s face rules and the two rasterisers — `0x4C7580` + `0x4C7310` for a textured quad, `0x4C0330` for a flat face — integer for integer, clipped to the packet's `vp_addr` exactly as the engine's context rect is. The result is **runs** (32 B): a row's pixels that take one texel of the unit atlas, or one palette index where the face is flat or the texel is the frame's key (which the span copies and the atlas marks a hole). A frame's runs stop at `TAGPU_FXM_RUN_MAX` (131 072, 4 MB), the budget every holder of them shares — this arena, the posed pass's hand-over, and the unit pass's host-visible storage buffer, one per swapchain image, so 2 + nimg copies: 20 MB at the full budget with three images; a model past it is refused and its record taken back whole |
| `tagpu_native.c` | sizes the effects band from the packet's own bound on the frame's models, before any key is fixed, and appends one draw per model to the posed pass's list after its units (`tagpu_posedraw_fx`) |
| `tagpu_posedraw.c` `FXVS` | a vertex stage with no vertex input: six vertices a run, read out of the frame's pose buffer by index, placed in frame px through the world's own zoom transform, at the model's key |
| `tagpu_vk_unit.c` `s_pipeFx`, `RS_FX` | that stage with the unit program's fragment stage, no cull (a run is a rectangle; the rasterisers already decided which faces paint), depth `LESS_OR_EQUAL` and written, so a model's later runs win over its earlier ones as the engine's face order does. Recorded after every unit body and before the effects pass's sprites. **A model's window is the two blocks `RS_FX` binds** — the body's vertex block and its fragment block, 448 B on the reference device's 64-byte alignment against a unit's seven blocks and 1 536 B — laid out after every unit's window; each draw carries its window's offset (`uoff`) and the block writer walks the draw list, so the writer and every stage name one offset. **The pass never refuses or stands down for an effect**: the models are checked whole before anything is sized for them — runs past the budget or past the device's storage range, more models than the effects pass takes, no effects pipeline, a record whose runs are not inside the array — and a failure drops the frame's models and keeps its units, so the blocks sized are the blocks drawn. The units' size is taken first, exactly as a frame with no model takes it; the models' growth comes second and keeps the units' buffers until the larger ones exist (`slot_grow`'s `keep`), so a slot the models make too large to allocate keeps what its units need. That frame still loses every effect — the effects pass draws none of a frame whose models this pass drops — so the share it would not grow by (blocks, runs, draws) is owed (`s_fxShort`): `fx_ready` answers no while it is, and each frame that draws units grows its own slot by it, a bit a slot, cleared when a slot is given back or its growth fails; once every slot holds it the models are asked for again |

**What a model takes from the unit program**, which is why the runs go through it rather than a
program of their own: the unit atlas's texels (so a model's texture lookups spend the unit
atlas's allowance, as a unit's do); the neutral face shade, because neither effects draw shades a
face; the fog at its effect's anchor, the rule the effects pass applies to its sprites; Classic++'s
restored art and its lighting with a level normal (the lambert is the level constant); and the
Gamma once, at the world composite. No shadow is sampled and no waterline clips it
(`uShadowOn = 0`, the water and dig heights out of reach): the engine draws neither for an effect.

**The depth order is the engine's paint order.** The effects band starts at `encFx = fxKey − 1.5`
and holds `2N + 2` keys for `N` models; model `k` takes `encFx + (2k + 1)·step`, a sprite or a
line after `k` models `encFx + 2k·step`, and every flash the key the explosion walk starts at.
The step is never below `16·2⁻²³` of the depth scale and the band grows to hold it, so adjacent
keys stay apart in the depth buffer (D24 or float, §2.91); at the default six the air band is where it was
(`airKey = fxKey + 12`). [Effects](../effects.html) §4 has the sequence.

**An effect is drawn whole or not at all, across two passes.** Inside `tagpu_fx.c` the record's
bracket takes back every part of a record that lost one — a model the packet did not carry, a
texel the unit atlas's allowance has not painted yet, a model past the frame's bound or the run
arena — with its sprites, its lines and its keys. Between the passes the hand-over's `nmodels` is
the handshake: the unit pass draws the frame's models all or none (`s_nfx == nfxRec`) and tells
the effects pass through `tagpu_vk_unit_fx_count`; the effects pass draws only a frame whose
models the unit pass draws (`h.nmodels == tagpu_vk_unit_fx_count`), and the unit pass records
`RS_FX` only when `tagpu_vk_fx_models_ok` says the effects pass is drawing that frame. Both
passes prepare before either records, so the answer is known when it is used. A model the posed
pass cannot carry is dropped there without touching the units' refusal count, and the handshake
turns it into a frame without effects. Before the gather, the native pass asks whether this frame's models
can be drawn: `modelsOn` is the posed pass's readiness **and** the unit pass's
(`tagpu_vk_unit_fx_ready`), so on a frame the unit pass will not draw only the records that
carry a model are taken back and every other effect draws. The unit pass answers no when it
has refused, is owed a teardown or has no effects pipeline; when it holds a hand-over it did not
draw (`s_fxHold`, set when a hand-over with a record is taken and cleared when `prepare` draws
it); while it owes a models' share a slot would not grow by (`s_fxShort`, below); and when
the frame's **forecast** stands it down. The forecast (`TAGPU_VKFXASK`) is the five stand-downs
that persist from one frame to the next, as the gather can know them: cast shadows on a device
that will not filter the depth map, the draws the hand-over will not carry (this frame's count
so far plus the last frame's whole), a fog overlay the frame samples with a grid the posed pass
could not carry, the scaffold overlay armed, and a texel mirror — the unit atlas's, the
palette, the face-shade table — not there yet, read through the hand-over's own readers
(`tagpu_posedraw_mirrors`). It is put through the same `frame_down` that `upload_draw` applies
to the hand-over, the mirrors through one predicate (`mirrors_there`), so the two cannot
disagree about what stands a frame down; and a mirror, once there, stays for the process's
life, so the forecast can be pessimistic about one (a mirror that arrives after the gather)
and never optimistic. It is asked only when the posed pass is ready.

**The thread split.** The engine's models are read on the game thread alone, by the publisher;
`tagpu_fxmodel.c` reads the packet it is handed and the unit atlas through
`tagpu_r3d_atlas_texels` — whose miss paints the atlas from the engine's GAF frame at the address
the packet carries, on the render thread under the fence the pose bake reads behind
(`tagpu_render3do.c`'s allow-list line) — and names no engine memory itself
(`thread-split: clean`, 32 files). `0x4B6CC0` is pure — it reads its two inputs and
one constant and writes `dst` alone — which is what lets the publisher call it (engine map,
*The effects models*).

**MEASURED 2026-09-24** (1024×768; the oracle is the engine's own draw with `fxown`'s skip off,
the game paused, the eye pinned):

| check | result |
|---|---|
| model pixels at 1×, Classic | `fx-rockets` **28 of 28, 18 of 18, 2 of 2** exact, and 28 of 28 again on the build first reviewed (its 16 uncovered engine pixels the two rockets' ground-shadow blobs); `big-battle` **1292 of 1352**, the other 60 all where the engine's finished frame shows a health bar drawn after the effects. On the build with the bounds, the reach and the drops below: `fx-rockets` **15 of 15** and **19 of 19**, `big-battle` **487 of 492** and **587 of 588**, the others under a health bar's outline or at a sprite's edge the engine draws after the models; merged over main `b09ec0d`, `fx-rockets` **20 of 20** and `big-battle` **968 of 968**; on the two-block build (two blocks a model, the forecast), `fx-rockets` **13 of 13** and `big-battle` **1381 of 1459**, the other 78 all under a health bar's fill or outline the engine draws after the effects; on the final build, `fx-rockets` **28 of 28**. Of the 290 engine effect pixels neither capture inks, 73 are black — which the capture cannot tell from its clear — and the presented frame shows every one of them exactly; 198 are the half-alpha ground-shadow blob; 19 lie at a sprite's or a flash's edge, 29 px or more from any model |
| the band order, in the presented frame | where only a model has ink the frame shows it, **857 of 857**; where a model and a sprite overlap the frame shows whichever the engine painted last, **247 of 257** — the 10 others sit under a flash and take its additive RGB approximation of the LHT remap |
| Classic++ | the same 1192 pixels covered; 812 take a restored colour |
| zoom 0.49× and 2.35× | every model where the world's own zoom transform puts it: within 0.15 px at 0.49× and under 1 px at 2.35× |
| a forced texel loss — a test build whose unit atlas answers "not painted yet" for every other GAF frame, then "refused" for all of them, 22 s each on `big-battle` | 38 106 and 50 534 records taken back whole; records left half-drawn **0**. Again on the review build: 60 628 and 127 767, half-drawn **0**; on the two-block build, 10 s each: 48 282 and 59 001, half-drawn **0** |
| forced overflows, the same test build, `big-battle` in play, 20 s each | the five tables held to 256 rows and 8 KB: 96 762 records taken back whole; the slot cut where the model tables begin: every model record taken back; the run budget held to 256: 42 630 taken back, the rasteriser's reason 3 (no room); half-drawn **0** in all three, the packet's `viol=0` (not one packet refused), the census `unit=1 fx=1` throughout. The unit pass's own run budget held to 16, and its slot sizing failed on purpose whenever there are models: the census `unit=1 fx=0` — the units drawn, that frame's effects not — and no refusal. Again on the two-block build, 10 s each: the same answers, half-drawn **0** and `viol=0` throughout, and with the models' sizing failing every frame the units' own buffer still grew with them (515 units, 791 040 B = 515 × 1 536). On the final build, the models' sizing and every retry of the share failing on purpose, 14 s: **1** frame without effects, the one it began on; then 193–344 sprites a heartbeat with the model records taken back, the census `unit=1 fx=1`; the lever removed, every slot grew by the share and the models came back (89–159 a heartbeat), half-drawn **0**, 0 of 320 000 block reads wrong |
| the unit pass refused on purpose (test build, 25 s) | the census `unit=0 fx=1`: the effects pass draws (48–141 sprites and up to 84 flashes a heartbeat), 49 915 model records taken back whole, half-drawn **0**. On the two-block build: one frame loses its effects (the one the refusal begins on), then 160–270 sprites a heartbeat with 44 585 model records taken back, half-drawn **0** |
| the stand-downs, the two-block build, `big-battle` in play, a test build counting every frame whose models were carried and not drawn | the scaffold armed, 15 s: **0** such frames — 250–390 sprites a heartbeat, the models taken back (the forecast). A draw the hand-over does not carry, forced where the native pass counts its truncations, before the gather: **0**. The same forced after the gather (the posed list's growth site), which the forecast cannot see on the frame it begins: **exactly 1** (frame 19 921), then the models held back and 66–125 sprites a heartbeat; lever removed, the models back. Refused (above): 1 |
| the unit atlas's mirror held back on a view of models alone (the final build, `big-battle` with the posed pass drawing no unit, 12 s) | after the onset **0** frames without effects, 128–270 sprites a heartbeat and the models taken back; the onset cost one, which is the lever's — it withdraws the mirror inside a frame, between the forecast and the hand-over, and nothing in the code ever withdraws one. With the forecast blind to the mirror, as the two-block build's was: **every other frame** without effects, 270 of 540. The lever removed, the models back |
| the model windows, read back (the same test build, `big-battle` in play) | every draw's `uEnc` read from the vertex block its stage binds (`uoff`) and `uFog` from the fragment block (`uoff` + the model's or the unit's fragment offset), against the draw's own record: 384 667 unit draws and 95 333 model draws, **0** wrong; again with 10 000 model windows forced, 396 676 and 83 324, **0** wrong |
| a slot's uniform buffer at 10 000 models | before, 1 536 B a model: **15.36 MB** of model windows a slot, from the code at `aef646f`. After, 448 B a model: **4.48 MB**; MEASURED with the models' size forced to 10 000, the slot's buffer **5 435 392 B** with 622 units' windows (955 392 B) in it. Three swapchain images: about 33 MB less host-visible memory at that count |
| the reach, in a test build that posed every model anyway and checked every posed vertex | of about 2 million vertices on `big-battle`, **0** outside the margin their node's rest vertices give; of about 150 000 models the reach would have skipped, **0** with a pixel inside it |
| A/B against main, the same fixture and eye, `ss=2` | terrain (`static-terrain`), units (`selbox-facings` with `native.on=all`, and `pose-inventory`) and features (`feat-forest`) **byte-identical**; again after the review, merged over main `b09ec0d`: terrain, units and features **0 px**, and `pose-inventory` **0 px** against main's launches that land in the same state — one animating piece there (x 1603..1610, y 857..868) takes one of three angles by launch, 26–38 px apart, main's own launches included. On the two-block build, against main `b09ec0d`: terrain, units and features **0 px** of 3 145 728, and `pose-inventory` **0 px** against two of main's four launches, 38 px against the other two — main's own spread. On the final build: units **0 px**, `pose-inventory` the same 0 and 38 |
| the GPU frame, `big-battle` paused at a pinned eye, `--maxfps 0`, two launches each | the frame's p50 settles at 2.40–2.47 ms against main's 1.83–2.40 ms. Paused, the publisher poses nothing new — the gather is kept while the tick stands still — so this row says nothing about the game thread |
| **the publisher in play**, 1024×768, `--maxfps 0`, a test build timing the posing (once a tick) and each whole fill, 256 of each a line | `big-battle` at the battle, 100–200 models posed a tick: the posing p50 **128–182 µs**, p99 183–257 µs; the fill that gathers (once a tick) p50 **0.76–0.91 ms**, p99 1.18–2.33 ms, against **0.60–0.98 ms** and 0.90–1.64 ms with the posing turned off in the same session; the fills that reuse the tick's gather p50 0.29–0.35 ms. `limits-flood`'s first volley, about 1100 models posed a tick: the posing p50 **1.22 ms**, p99 1.92 ms; the gathering fill p50 **2.68 ms**, p99 4.19 ms, against 1.40–1.46 ms and 3.79–3.84 ms without the posing (another launch of the same volley); at 1920×1080, 2.34 ms and 4.12 ms. **The reach skips nothing at a battle on screen** — at 1024×768 the zoom floor's window holds all of `big-battle` — and with the camera 2500 px off it every model is skipped: the posing p50 34–44 µs against 107–154 µs posing them all |
| the run budget's headroom | `limits-flood`'s volley peaks at **41 819** runs (717 models on screen) at 1024×768 and **61 630** (962 models) at 1920×1080, against 131 072 |
| the log, a minute of `big-battle` in play | clean |

**The heartbeats.** `fx:` gains `models= runs= (lost= why= of cap )` — drawn, their runs, taken
back, the rasteriser's last refusal (1 not carried, 2 texel not painted, 3 no room), the frame's
bound; `packet:` gains `models=N/Vv lost=` — the models carried and their vertices, and the
running count of models the publisher could not carry (a node that did not validate, a face it
could not carry, past the tables' bound, or no room). A frame whose models were dropped says so
once: `posedraw: N effects model(s) could not be carried …`, `unit: … the effects are not
drawn, the units are` (the whole check), or `unit: the slot would not take … the models are
left out until every slot takes them` (the share, owed from then on).

**What it does not do.**

- **At zoom-in a model is magnified engine pixels**, as the rest of the effects are: the runs are
  the engine's pixels at its resolution, and the zoom scales them. A model drawn at the display's
  resolution is geometry again, which is the measured alternative above.
- **The flash is still an additive RGB approximation** of the LHT remap, and still draws before
  every sprite of the frame where the engine draws it after the projectiles (effects §4). Both
  predate this work; the 10 overlap pixels above are the approximation's.
- **Past the engine's own capacities the engine's picture is not reproduced.** `0x46BAE0`
  rotates and projects into heap blocks of 200 vertices and gathers a face into one of 20
  points; `0x4211D0` gathers a face into 25 points of its stack and projects into the rest of
  its frame, room for exactly 2000 vertices with the next one over its return address (engine
  map, *The effects models*). Neither tests a count, so past them the engine writes over its
  neighbours and draws what that leaves. The packet carries such a model as the arithmetic
  describes it, uncorrupted, up to 2048 vertices, 512 faces and 32 points a face; past those the
  model is not carried and its record is taken back whole, and a face taller than 65 536
  scanlines refuses its model the same way, whole record and all. No stock model reaches any
  of them.
- **The reach and the projection keep the whole pixel.** The engine projects a model from its
  position less the eye's and cuts each pixel to 16 bits (`movsx` at `0x46BB3B`, `0x46BB43`,
  `0x46BB51`), so a model 32 768 px or more from the eye is placed modulo 65 536 — off the
  screen, until one about 65 400 px away lands back on it. The publisher's reach and the
  rasteriser's projection keep the whole value, so such a model is culled, or drawn where it
  is. The two agree while the model and the eye are closer than 32 768 px, which every stock
  map guarantees: the largest, Seven Islands, is 20 480 px square.
- **A frame past the run budget loses the models past it**, each with its whole record — sprites,
  lines and all — and keeps the ones before. The budget is what the 2 + nimg copies of the runs
  may cost in a 32-bit process (20 MB at three swapchain images), 2.1× the largest frame
  measured (the flood's volley at 1920×1080); a battle at the raised caps seen whole, zoomed out
  on a large screen, can pass it.
- **A stand-down the gather cannot foresee costs one frame of effects.** The five that persist
  are forecast (above), and a frame they stand down loses its models alone — as does the one
  frame after uncarried draws stop, the forecast still counting the last frame's. Any other — a
  frame whose atlas tiles find no staging, a unit whose bake mirror was evicted, the refusal,
  and a slot the models' share will not grow by — costs the frame it begins on all of its
  effects, and the models are then held back until the pass draws again or every slot holds
  the share. **On a view of models with no unit** a frame with nothing to draw ends the hold,
  or the models would never come back, so while such a stand-down lasts every other frame
  loses its effects; of the unforeseen ones only the staging's can last there, and only while
  a host allocation keeps failing. The models' share is grown only by a frame that draws
  units, so on such a view the models stay out until a unit is in view. A unit pass that has
  refused for the session, or has no effects pipeline, takes only the models back
  (`modelsOn`).
- **Fog is decided at the effect's anchor**, for the model as for its record's sprites and
  lines (`vWorld` is the anchor; the effects pass has decided it there since G13b, and a
  record's parts fog together). The engine's overlay decides per pixel, so a model that crosses
  a fog cell's edge can differ from the engine by the part across it. No scene of this work had
  a model on such an edge. The fix is per-pixel fog for every effect, sprites included, and it
  is the next change to the effects pass.
- **Classic++ has no engine oracle**: its models were checked for coverage and for taking the
  restored art, not against a reference picture.
