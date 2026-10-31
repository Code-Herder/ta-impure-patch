#ifndef HUDSTATEJSON_H
#define HUDSTATEJSON_H
/* hudstatejson -- the mockup's replay HUD state (tools/hudart/mockup/index.html `state()`, and
   tools/wiki/mockup/frame.js hudState) read into a HUDSTATE, for the checks that lay the same state
   out in C: tools/hudart/laycheck.c and tagpu/ddraw/tests/wikiframetest.c. Native, pure C99. */
#include "tagpu_hudlay.h"
#include "tagpu_json.h"

/* `S`, the state's JSON object, into `st` (cleared first), every field the mockup names. Two are
   the caller's, since the state does not carry them: `px`, the device pixels a unit, and
   `fork_max`, the fork limit. A seat's or a group's colour is "#rrggbb" or "rgb(r, g, b)". */
void hudstate_json(const TJV* S, HUDSTATE* st);

#endif
