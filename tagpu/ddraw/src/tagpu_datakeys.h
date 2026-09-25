#ifndef TAGPU_DATAKEYS_H
#define TAGPU_DATAKEYS_H

/* tagpu_datakeys — TADR section C's new data keys
   (research/notes/tadr-port/data-keys.md).

   The unit-key reader: one observer inside the FBI loader 0x42BF40, at
   0x42BF97, reads the keys this module owns out of the FBI section the loader
   is holding, through the engine's own TDF reader, and keeps one record per
   UnitDef slot. Installed at attach for the process, byte-matched; a mismatch
   leaves the image untouched and logs.

   The build ghost's piece mask (C1): which pieces a type's ghost hides. By
   default the pieces its COB `Create()` hides before anything else runs; with
   `PreviewPieces=` in the FBI, every piece that list does not name. Computed
   on the game thread, cached per type for the level, published in the frame
   packet as TAGPU_PK_GHOSTMASK. The render thread reads only the packet. */

#include "tagpu_packet.h"

/* DLL attach, before the engine runs: installs the FBI reader. */
void tagpu_datakeys_init(void);

/* GAME THREAD, inside an in-play packet fill only. The ghost mask of unit type
   `type`: 1 with `*out` filled when the ghost hides at least one piece, 0 when
   it shows every piece (no row). Bounded by the engine's own UNITINFOCount. */
int tagpu_datakeys_ghost_mask(unsigned type, TAGPU_PK_GHOSTMASK* out);

#endif
