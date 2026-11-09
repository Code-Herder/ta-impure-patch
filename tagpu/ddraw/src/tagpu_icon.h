#ifndef TAGPU_ICON_H
#define TAGPU_ICON_H
/* tagpu_icon.h -- the strategic icons, the render thread's half: which unit of
   this frame's packet is an icon, which model is hidden under one, and the
   icons in their draw order.

   RENDER THREAD ONLY. tagpu_icon_frame runs once a frame, before the unit
   gather (tagpu_native.c) and the marker gather (tagpu_mark.c), which both
   read what it decided in the same frame on the same thread, so program order
   is the whole ordering. It reads only the packet and the published icon art
   (tagpu_icontype.h), never engine memory. A unit is named by its PACKET index
   throughout, the index both gathers walk. */
#include "tagpu_packet.h"

/* This frame's decisions, from the packet and the frame's own level `zoom`
   (the level the marker pass draws at, never the packet's diagnostic
   `zoom_applied`). `on` 0 -- the unit pass or the marker pass is not drawing
   -- leaves every unit a model and no icon. */
void tagpu_icon_frame(const TAGPU_PACKET* pk, float zoom, int on);
/* 1 when this frame decided icons at all (tagpu_icon_frame's `on`, and the
   packet carried a type table and the art is published). */
int  tagpu_icon_on(void);
/* tagpu_icon_on() as the last tagpu_icon_frame since the previous call left
   it, and 0 when none ran -- a frame that left the overlay before the native
   pass -- for the command record (tagpu_zoom_frame_end), which carries it to
   the game thread's pick. Clears it. */
int  tagpu_icon_cmd_take(void);
/* Of packet unit `i`, this frame: the class icon's alpha (0 = no icon); 1 when
   its model is not drawn; 1 when it is cargo of a unit whose model is not drawn,
   and so neither a model nor an icon. 0 for an index out of this frame's
   units. */
int  tagpu_icon_alpha(unsigned i);
int  tagpu_icon_hidden(unsigned i);
int  tagpu_icon_suppressed(unsigned i);
/* Either of the two above: the unit has no model on screen, so its health bar
   and group digit are not drawn either (BAR fades them out there). */
int  tagpu_icon_hides(unsigned i);
/* Queue packet unit `u` (index `i`) at its anchor (ax, ay): when `dot` is 0
   its type's icon -- the mod set's picture for it, else its class's shape --
   else a radar contact's, at every level under `radardots=always` and under
   the same distance rule otherwise: the set's NONE picture, else the radar
   dot, its owner's radar logo frame -- with the hover ring queued over either
   when it is the packet's hovered unit. `sel` fills a type's icon white. 1 when queued; 0
   when the rule or the art gives it nothing, or the list is full (counted). */
int  tagpu_icon_push(unsigned i, const TAGPU_PK_UNIT* u, float ax, float ay, int sel, int dot);
/* From the marker gather: the queued icons sorted into BAR's order -- by
   layer, then far to near, then packet index -- and emitted through
   tagpu_mark_emit_icon. */
void tagpu_icon_emit(void);
#endif
