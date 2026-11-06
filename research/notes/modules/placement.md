# Several buildings in one gesture

Shift-drag lines, rectangles and rings of buildings, and their spacing.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.109 Several buildings in one gesture (`tagpu_place.c`, `tagpu_placegeo.c`) — 2026-10-25

With a building armed, Shift + left-drag lays a line of it (Ctrl: held to the dominant axis),
Shift + Alt a filled rectangle in serpentine order (Ctrl: hollow), and Shift + Ctrl over a unit, or
over a queued build of a selected builder, a ring round it; the release with Shift held orders it.
The shapes are BAR's (Recoil's `GetBuildPositions`) in TA build cells, ported to exact integers
(`tagpu_placegeo.c`, engine-free, self-tested by `tests/placetest.py`). The take-off rule is there
too, as `place_take_offs_one`, which the in-game wiki's simulation (`tagpu_wikisim.c`) also calls. Behaviour, semantics and
gaps: [quality-of-life](../tadr-port/quality-of-life.html), *What Impure does*. The engine side: the
engine map's *The placement's mouse path, and several buildings in one gesture*.

| Seam | Mechanism |
|---|---|
| `0x4197D0` | **After-observer** (`tagpu_detour_observe` with a `before` that keeps the return address and an `after`), the same nine stolen bytes `83 EC 14 8B 15 E8 1D 51 00` as the resource module's before-observer, **chained after it and last on the site** (the chain rule, `tagpu_detour.h`): `tagpu_place_init` runs right after `tagpu_resource_init` (`dllmain.c`). After the engine has written the rectangle, the altitude and bit 6 for the current record, it evaluates the shape from the press site to the engine's cursor site and judges each site |
| `0x4198C6`, `0x47C780`, `0x47D820` | Read at install: the `E8` at `0x4198C6` and its target, which is stock `0x47D2E0` or queued_site's `build_site_test`. Each site is judged by calling that target with the build cursor's own arguments, so a site is red exactly when a single click there would be refused; altitude from `0x47C780` (free) or `0x47D820` (refused). `0x51E684`/`0x51E688` saved and restored around every loop. A self-check compares the call's verdict at the cursor site with bit 6 (`place: mismatch`, counted at level end) |
| `tagpu_shield.c` key paths | **Spacing keys**: Alt+Z / Alt+X with a building armed step the armed type's spacing (`tagpu_place_key`) and go no further — on the real path as `WM_SYSKEYDOWN`/`WM_KEYDOWN` only while the shield lets the player's keys through, on the injected path in `deliver_key`; outside mode 14 they reach the game. Game thread (the window procedure) |
| `0x499200` | **Before-observer on the in-play handler's entry** (stolen `A1 E8 1D 51 00 83 EC 18`), the router's (`tagpu_route.c`, shared with the reclaim sweep, §2.110), which calls `tagpu_place_entry`: a record handled in a mode other than 14 ends the gesture, so a disarm the other two hooks cannot see (mode 1 skips both) never leaves a gesture for a later re-arm to commit |
| `0x4993BF` | **12-byte route stub** (`E9 rel32` + seven `90`), the router's (`tagpu_route.c`, shared with the reclaim sweep, §2.110), written through `tagpu_detour_write` after all 22 bytes `0x4993BF..0x4993D4` match. `pushad`, `route_button(record copy, message)`, `popad`; in mode 14 `route_button` asks `tagpu_place_button`, and its answer is the way taken: 1 → the frame `0x4995B8` (a Shift press starts a gesture instead of placing; a release commits it), 2 → `0x4993CB`, stock's click on this record (a one-site gesture at the release cursor is stock's own Shift-click: its toggle, sound, recorded click and bit 5), 0 → the two original instructions |
| `0x48CD80` | Called by the evaluation, with Shift and Ctrl held, for the ring's unit: the engine's hover query, which the handler skips in build placement; its answer is bounded as a unit slot |
| `0x438760` | The two build actions `MOBILEBUILD` / `VTOL_MOBILEBUILD` by the engine's own name strings (`0x502654`, `0x502640`), as `0x419670` asks |
| `0x439F80`, `0x43ADC0` | The commit (inside the click routing, where the stock click calls its root), from the same take-off rule the preview's colours come from (`take_offs`, which calls `place_take_offs_one` once per builder over its run of the snapshot): per builder, each queued node of the builder's build action is taken by one non-red site at most — first every site takes the node at its own position, then each site without one the first within 16 units that no site took. The taken nodes are removed through `0x439F80` in descending place in the chain, the head last, each re-found by its place and removed only if it still carries the snapshot's action, position and flag (else counted `stale=` and left queued); then every other non-red site is appended with Shift (`0x43ADC0`) while the builder's chain holds fewer than 1024 nodes |
| `0x47F1A0` | `oktobuild` (`0x509660`) once a committed gesture that added or removed an order, as `0x498FA4` plays it per click |
| `tagpu_packet_pub.c` → `PK_PLACE` | The sites copied by value (≤ 256 × 28 bytes: the build rectangle in `0x4197D0`'s layout, the type, the GUI colour index `0x0A` placed / `0x04` refused / `0x0E` toggles a queued build off). The acquire bounds the count, each colour, each coordinate (±`0x100000`) and each type (`< udef_count`). An overflow sets `TAGPU_PK_TRUNC_PLACE` (`0x4000`) and the square stays replaced |
| `tagpu_mark.c`, `tagpu_native.c` (Vulkan) | The marker pass draws each site as the build square (its gate, projection, ±`0x100000` drop, normalise-then-inset; `square_gate`/`square_emit`, shared with the cursor's square) from a bucket of its own, and skips the one square; the ghost pass draws one build ghost per site instead of the cursor's. Counters: `mark: … place=N placered=R placetake=T` (sites drawn, of them red and yellow) under `tagpu_mark.on=log`, `ghost: … sites=N` |
| `0x469EC5`, `0x469F1E` (GDI, `tagpu_markown.c`) | The existing redirect of the square's two `0x4BF8C0` calls draws each site's rectangle instead: the engine's rectangle moved by the site's offset from `main+0x2C92..0x2CA6`, so the projection and vpwide's widening stay the engine's. Needs `tagpu_markown.on` |

**Threads and lifetimes.** All of the module's state is game-thread state, written and read inside
the in-play handler and DrawGameScreen; the render thread sees only the packet's values. The unit
array, the player's block, slots and def indices are bounded against their engine counts; every
order-chain walk is bounded (16384) and stops at a node another unit owns; builders are held as
slot indices and re-derived before every root call, and no node pointer survives `0x439F80`. The
builder set is `0x419670`'s (selected, builder def) narrowed to alive, not excluded and owned; like
`0x419670` it does not consult CANBUILD. A commit is capped at 4096 orders (sites × builders); the
preview shows the cut shape. A puppet seat refuses every hook. `thread-split.allow` lists the file
`publisher presentation` (the owner's decision).

**Replay.** The module is presentation: it declares no simulation state, and a commit's effects are
ordinary order nodes the recorder keeps. A one-site gesture goes through `0x4995B3` and records its
click as stock's; a multi-site gesture records none. Its sound has the owner-approved
`replay-absent.tsv` row `0x47F1A0 dll * absent` ([recorder
completeness](../tadr-port/demo-recorder-completeness.html)). Test group `place-basic`
(`scenarios/place-basic.json`, `tools/place_regression.py replay`).

**A zoomed Shift-click.** Between the press and its release IdleTick hands the handler the move
made before the press (the ring is empty), and the gesture ends on a move without the button that is
not that one. The comparison skips x and y: zoomed, the cursor polls rewrite them in screen space
under a move message that carried the unzoomed position (engine map, *After a press, IdleTick
re-reads the move made before it*). Compared whole, 25 of 38 Shift-clicks at about zoom 0.35 lost
their release and ordered nothing on the reference setup; none of 8 do with dwords 2..5 compared.
Nothing else reproduced it: neither tacli's injected input nor X-level clicks on a private Xvfb
display at zoom 0.35 and 0.6 (16 presses, none lost), so no regression case covers it.

**Open:** Build Split is not built; GDI shows the sites only with `tagpu_markown.on`
(without it the engine's one square); the replay overlay shows no click for a multi-site gesture;
no facing. The take-off rule (yellow) is the module's reading of `0x43AFC0`, not the engine's
call, and is checked only by the regression (cases (h) and (n)) and by `placetest.py`'s table
cases of `place_take_offs_one`. The preview and the commit
apply the one rule (`take_offs`) and agree for the same queues; the release judges again against
the queues as they stand at the release, so a change since the last drawn frame — a tick
finishing or starting a queued build, a builder dying or the selection changing (the order
budget), a nanoframe turning a site red — changes its result, and a release over the side panel
acts on a snapshot newer than the preview: the evaluation runs only from `0x4197D0`, which the
handler calls only with `main+0x2CC6` bit 1 set (`0x499236..0x499241`), while the route at
`0x4993BF` and the release reach `tagpu_place_button` anywhere. `stale=` counts only a queue that changed
inside the commit itself.
