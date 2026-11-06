# The reclaim sweep

A ring of RECLAIM orders from one drag.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.110 The reclaim sweep (`tagpu_sweep.c`, `tagpu_sweepgeo.c`, `tagpu_route.c`) — 2026-10-28

With RECLAIM armed (cursor mode `main+0x2CC3` = 12, which only the RECLAIM branch of the orders
menu writes) on the Vulkan lane, a left drag on the ground draws a ring; its release orders the
selected reclaimers to take the features of the chosen kind inside it — METAL (a def with metal,
`+0xF0` > 0), ENERGY (`+0xEC` > 0) or ALL — split nearest-first and balanced by queued work
(`tagpu_sweepgeo.c`, engine-free, checked by `tests/sweeptest.py` against a port of the mockup's
split). **Without Shift** the ring takes off, through the exact-removal root `0x439F80`, every node
a stock replace would (no `+0x42` bit 2) except a reclaim-family node on one of the ring's own
targets, which stays where it is; then it appends the rest, none twice, and the order disarms.
**With Shift** every order stays, the ring appends, reclaim stays armed and releasing Shift
disarms. A press and release under 7 screen px of travel is stock's click on that record, byte for
byte (it replaces the orders). A feature of the other kind shows the engine's "nothing" cursor and
a click on it orders nothing. Right press or Esc while a drag's left press is held ends the drag
and keeps the order armed -- held as the window procedure passed the buttons, so a release
already on its way to the engine still commits; otherwise both are stock's (disarm). While armed, a popup off the RECLAIM gadget's row
picks the kind, the gadget's quickkey cycles it, and a tag beside the cursor names it; the kind is
kept as `reclaimkind=` in `impure.cfg`. The sweep draws no queue markers of its own: what a ring
queued shows through the engine's order markers (`tagpu_order.c`) under Shift, as every queued
order does. On GDI reclaim stays stock. The engine side: the engine map's *The reclaim sweep's
engine side*.

| Seam | Mechanism |
|---|---|
| `0x4993BF` (`tagpu_route.c`) | **The router's 12-byte click-route stub**, which this module shares with the placement (§2.109): written at attach after all 22 bytes `0x4993BF..0x4993D4` match; `pushad`, `route_button(record copy, message)`, `popad`, the three ways of §2.109. `route_button` dispatches on `main+0x2CC3`: 14 → `tagpu_place_button`, 12 → `tagpu_sweep_button`, every other mode 0 (stock). In mode 12 a press on the ground (region bit 1) is consumed and is the ring's centre (`main+0x2CAA` of that record); the release past the threshold commits (1), below it is stock's click (2) |
| `0x499200` (`tagpu_route.c`) | **Before-observer on the in-play handler's entry** (stolen `A1 E8 1D 51 00 83 EC 18`), shared: it calls `tagpu_place_entry` and `tagpu_sweep_entry`, each of which ends its own gesture when the record is handled in another mode, or (the sweep) when it is a move without the button that is not the press's stale move |
| `0x499297` (`tagpu_route.c`) | **The cursor chooser's call, re-pointed**: its `E8 rel32` (after `push eax` `50` at `0x499296`) is matched as `50 E8 84 3F FF FF`, decoded to `0x48D220`, and its rel32 rewritten to `cursor_handler`, a stdcall wrapper of one dword that calls `0x48D220` itself and hand the answer to `tagpu_sweep_cursor`, which returns it unchanged outside mode 12. In mode 12 it moves the drag's edge, returns `0x13` over the popup, and returns `0x13` for a feature the resolver would reclaim that is not of the chosen kind — the value the handler stores in `main+0x2CBE` and stock's click reads in the same call. The chooser's other call, `0x491D36`, lies in `0x491CC0`, which nothing calls, and is left alone. Nothing else in the tree writes the call |
| `0x43F0E0` | The order resolver, mode 12, no target unit: asked once a cursor record for the feature under the pointer when the kind is not ALL (whether a click would take it), never by the ring's enumeration, which applies the same rule itself, and once per builder before each order (the order byte is the builder's own: `RECLAIM`, `VTOL_RECLAIM`, `RESURRECT`). A refusal at the commit counts `mismatch=`, the self-check of the ring's enumeration |
| `0x43ADC0`, `0x439F80` | The commit, inside the click routing where stock's click calls its own root: removals first (without Shift), each node re-found by a fresh bounded walk and removed through `0x439F80` only while it still carries the action, position and flags the first walk read (else `stale=`), in descending place; then every target appended with `0x43ADC0(action, 1, unit, NULL, pos, 0, 0)` while the builder's list holds fewer than 1024 nodes (`listcap=` counts a builder that filled up). `0x43AFC0` is not used: with Shift it toggles off a same-action node within 16 units |
| `0x4958C0` | Stock's click tail, called after a commit that issued or removed anything, with Shift as its argument: bit 5 of `main+0x2CC6` with Shift, else mode 1, bit 5 cleared and STOP's radio reset (which drops RECLAIM's armed plate). It has no caller in the image; `main+0x531` and its `+4` are tested first because it reads them unchecked |
| `0x408090`, `0x485070`, `0x438760`, the feature grid | Read: the mapped test and the ground height per candidate feature, the three family names (`0x5016C4`, `0x501B68`, `0x5052F0`) once a level, and the grid `*(main+0x14287)` with its W×H, defs and count read and bounded every call (the hop's anchor bounded too, which the engine's own lookup does not) |
| `tagpu_shield.c` key and button paths | `WM_CHAR` (real, injected through `deliver_key`, and `WM_TAGPU_CHAR`) → `tagpu_sweep_char`: Esc during a drag ends it, the RECLAIM gadget's quickkey (matched as the engine matches it, through `tolower`/`toupper`) cycles the kind while armed and goes no further — the engine would toggle RECLAIM off. Not while a modal screen, the chat or a textfield owns the keyboard (`main+0x37EBE & 0x865`, `main+0x2BEE & 0xE0`, `main+0x57D`). Buttons (real while the shield lets them through, injected through `deliver_engine`) → `tagpu_sweep_mouse`: a right press during a drag, and a left press on the popup as it was last drawn, are taken with their releases before the engine has a record of them (the right press's engine path `0x4993AC` is the recorder's). Focus loss → `tagpu_sweep_cancel`. Game thread |
| `tagpu_packet_pub.c` → `PK_SWEEP`, `PK_SWEEPITEM` | At most one header (flags RING / POPUP / HOVER / ARMED, the kind, the hot row, the art style, the popup's and the tongue's screen rects, the ring's centre and radius, the readout's counts and sums, four palette indices: 224 METAL, 208 ENERGY, the GUI green `gui[0x0A]` for ALL) and its items (20 B each: up to 256 ring points, up to 1024 bracketed targets, the hovered feature), copied by value. The acquire bounds every field (`tagpu_packet.c`); an item table rides only beside its header; a cut sets `TAGPU_PK_TRUNC_SWEEP` (`0x2000000`) |
| `tagpu_mark.c` `gather_sweep` (Vulkan) | The marker pass draws the ring (dashed 6/4), the brackets (the feature's drawn sprite, one px outside it; a 3-D wreck by its footprint and height) and the readout (`[KIND]`, counts, `+M METAL +E ENERGY`, or `NO … HERE`) in the cursor layer with fog off: 3 device px of colour with 1 px of black each side at any zoom, HUD scale and supersampling. Counter `mark: … sweep=N swover=V` under `tagpu_mark.on=log` |
| `tagpu_gui_surf.c` `sharp_sweep` (render thread) | The sharp layer draws the popup (plate, tongue, three rows) and the cursor's kind tag from Impure's art (`art/ui/<style>/sweep-*`, put into the UI atlas on first use) before the cursor, at the packet's rects times the layer's scale; the tag at the cursor's device point + (13, 20) × `cursorscale=`. A style without its own entries falls back to retail |
| `TAGPU_CMD_SWEEP` in the command record | **Render thread writes, game thread applies.** `sharp_sweep` records what it drew (`drawn`, `pass`, the popup's rect and the three rows' rects, game-frame screen px with the HUD scale applied); `tagpu_zoom_frame_end` copies it into the record (`tagpu_gui_sweep_cmd`); the game thread takes it where the record is applied, before every in-play draw (`tagpu_packet_pub.c` → `tagpu_sweep_take`), bounds each rect and keeps it as `s_drawn`. A LEVEL: a later record replaces it; a record whose sharp layer did not run (`pass` 0) keeps the last. The popup's clicks and the cursor's `0x13` over it are tested against `s_drawn` only, the popup the player saw |

**The popup's anchor.** The RECLAIM gadget is found again on every use by the engine's own rule:
the first button (id 1) of the top screen `[[main+0x531]+4]`, records 1..count (count bounded to
512), whose name contains `RECLAIM` (`0x49FE60`'s and the arming's substring test). The popup
(124×72 at x 129) has its middle row centred on the gadget's absolute rect (`ControlsAry[0]`'s
`+0x13/+0x15` plus the gadget's), clamped into the world band, and is magnified about the origin by
the HUD's q8 as the panel is. It is shown only when the gadget is active, not greyed, and lies on
the panel the player sees; otherwise there is no popup and no quickkey cycling, and the ring and
the filter work with the remembered kind. One line `sweep: anchor gadget=… rect=… popup=… q8=…`
when it changes (retail at 1024×768 with the HUD scale off, the line `sweep_regression.py popup`
expects: `ARMRECLAIM rect=5,300,55,31 popup=129,279,253,351`).

**Threads and lifetimes.** Everything but the sharp layer's drawing is game-thread state, read and
written inside the in-play handler, the window procedure and the publisher; the render thread sees
copies (the packet) and hands back values (the command record). The unit array, the player block,
slots and def indices are bounded by `tagpu_route_world` (the placement's view, moved to the
router); every list walk is bounded (16384) and stops at a node another unit owns; builders are
held as slots and re-derived before every root call; no node pointer survives `0x439F80`. The
feature grid and defs live from LoadMap to the teardown, both on this thread. A puppet seat refuses
every hook. `thread-split.allow` lists `tagpu_route.c` and `tagpu_sweep.c` as `publisher
presentation` (the owner's decision, as for the placement).

**Replay.** Presentation only: no simulation state is declared, and a commit's effects are order
nodes the recorder keeps. A click under the threshold goes through `0x4995B3` and is recorded as
stock's; a ring records no click. The module plays no sound; stock's arming plays `specialorders`
(`0x419F9D`) on the recording machine only, so the test group `sweep-basic`
(`scenarios/sweep-basic.json`, `tools/sweep_regression.py replay`) arms reclaim at most twice and
needs no `replay-absent.tsv` row.

**Open:** a 3-D wreck's bracket is its footprint and height, not its projected model; the popup's
art is a stand-in (`tools/sweepart.py`), owner art pending; the route dots of a queued reclaim
follow stock's rule (the hovered or GUI-shown unit only), the reclaim cursors show for every
selected builder under Shift; the bottom bar still names a feature of the other kind under the
pointer (`main+0x2CBC` is left alone); GDI has no sweep. The mod HUDs' RECLAIM buttons (Escalation,
Total Mayhem, ProTA, TA Zero, Twilight: `(7,479)` in the file, `(7,607)` on screen, from a scan of
the setups' `.gui` files, 2026-10-26) are covered by the anchor rule but were not run in game; on a 600-high screen a 640-tall panel puts RECLAIM off
the surface, so no popup there [INFERRED from the rule, not run].
