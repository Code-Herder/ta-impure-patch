#ifndef TAGPU_RANGEOWN_H
#define TAGPU_RANGEOWN_H
/* tagpu_rangeown — the weapon range rings' game-thread half: which rings show,
   of what class, around which unit. tagpu_range.h is the render half and the
   design; this file answers for the packet's PK_RANGE / PK_RING tables.

   WHAT SHOWS, as BAR's two widgets decide it (tools/tascene-ranges.js is the
   reference the rules were ported from):
   - the attack rings of the selected units (at most 358, BAR's
     selectionDisableThreshold), of the unit under the cursor, and the build
     rings of the player's builders while a building is armed -- under Shift
     only when `rangeshift` is on;
   - the defense rings of every structure with a weapon, by side and class
     (`rangeally`, `rangeenemy`), and those of the building being placed, which
     also light every allied ring of its classes;
   - per unit type and side, the rings alt+. and alt+, leave on;
   - the radar, sonar and jammer coverage of the viewed player and its allies,
     always, under `sensorview`: every unit of theirs alive, not dying and
     activated -- the radar pass 0x467440's emitter test for radar and sonar;
     the jammers act in the other players' passes. An enemy's coverage is
     never published;
   - while a building is placed whose type will emit once built (it switches on
     when built or the player can switch it on), its radar, sonar and jammer
     coverage, with the existing coverage of the same kinds, under
     `rangesensor`.
   The weapon rings are under `ranges`; the sensors are not.
   An enemy or allied unit counts only while the engine shows it to the viewed
   player (UnitInPlayerLOS 0x465AC0, the test HotUnits takes).

   ALL OF IT ON THE GAME THREAD, inside the engine's in-play draw: every engine
   field is resolved to a number in the record, so the render thread reads
   none. Presentation only -- nothing here is read by the simulation, and
   nothing enters a recording. */

#include "tagpu_packet.h"

/* 1 while the weapon rings are on (`ranges`, default on) and this lane draws them:
   the Vulkan backend, until its range pass refuses. The order markers' port of
   the engine's ShowRanges circles (tagpu_order.c) draws them only while this
   is 0: the rings replace them, and switching the rings off -- or a lane that
   cannot draw them -- brings them back. */
int  tagpu_rangeown_on(void);

/* The render thread: the range pass cannot draw on this device (no stencil
   plane, a pipeline that would not build). Sticky until `forgive`, which the
   pass's teardown calls when it does not leave the pass refused. */
void tagpu_rangeown_refuse(void);
void tagpu_rangeown_forgive(void);

/* Inside the in-play draw (tagpu_packet_pub.c fill_frame): the header and at
   most `max` rings, weapon and sensor. Returns 1 when a ring shows; `*cut` is set when more
   wanted to. Every record's `uidx` is 0xFFFF: the publisher maps `slot` onto
   its own unit table. Nothing is filled, and Alt+. / Alt+, are not taken, on
   a lane that cannot draw rings. */
int  tagpu_rangeown_fill(TAGPU_PK_RANGE* h, TAGPU_PK_RING* out, unsigned max,
                         unsigned* n, int* cut);

/* The level's end, on the game thread, before its arrays are freed: no key is
   taken until the next level's first in-play draw. */
void tagpu_rangeown_level_end(void);

/* Alt+. / Alt+, (VK_OEM_PERIOD / VK_OEM_COMMA) in play: steps the selected
   unit's per-type ring choice, BAR's cycleUnitDisplay. 1 when the key is
   taken -- TA would otherwise page the build menu with it. `alt_known` 1 when
   the caller saw Alt held (a WM_SYSKEYDOWN). Game thread. */
int  tagpu_rangeown_key(unsigned vk, int alt_known);

#endif
