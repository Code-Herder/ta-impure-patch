#ifndef TAGPU_UI_H
#define TAGPU_UI_H

#include "tagpu.h"

/* tagpu_ui — snapshot the live GUI gadget tree, on demand.

   The read half of `tacli ui`: a text description of whatever screen the game is
   showing, so a menu can be driven by gadget name instead of by screenshot and
   guesswork. Full reverse-engineering behind this lives in
   research/notes/gui-gadgets.md.

   OFF BY DEFAULT and one-shot, exactly like tagpu_peek: nothing happens until
   `tagpu_ui.trigger` appears next to the exe. It is deleted, the walk runs once,
   and `tagpu_ui.json` is written atomically (tmp + rename) so a reader can never
   see a half-built file. Probed every 5th frame — snappier than peek's 15 because
   the CLI polls this in its auto-wait loop.

   The chain, all read-only and pointer-guarded:

       main  = *(void**)0x511DE8
       gi    = main + 0x519                (GUIInfo)
       top   = *(void**)(main + 0x531)     (gi->TheActive_GUIMEM)
       ctrls = *(void**)(top + 0x04)       (ControlsAry)

   ctrls[0] is the panel record — its name is the screen name and its +0xB6 is the
   gadget count. Gadget i sits at ctrls + i*0x15B (pack(1), 347 bytes). Only the
   top GUI is interactive; the ones below it are reported by name only, as a
   breadcrumb.

   Gadget coordinates are relative to the panel record's own origin, in game
   space — the space tacli's injected clicks take, with no scaling anywhere.
   Shell menus are full-screen panels at (0,0) so their gadgets read as
   absolute, but the in-game build panel sits at (0,128) and everything in it
   shifts down by that. The emitted rect and click point are already in screen
   space; "panel" keeps the raw origin. */

void tagpu_ui_frame(const TAGPU_FRAME* f);

#endif
