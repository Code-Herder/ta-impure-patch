#ifndef TAGPU_WIKIFRAME_H
#define TAGPU_WIKIFRAME_H
/* tagpu_wikiframe -- the in-game wiki's shell laid out: the wiki button in the top bar, and the
   window -- its frame, its title, its close button and its body -- as the mockup places them
   (tools/wiki/mockup/: wiki.css #topbar .tb and #win, wiki.js GEOM.kit). What goes inside the body
   is the page's (the later landings); this file gives the body's rectangle and draws it as one
   empty panel until then.

   A PURE FUNCTION OF ITS INPUTS, like tagpu_hudlay.c: no engine memory, no global, no clock, no
   Windows header, so the render thread's frame and a native tool get the same rectangles for the
   same state, and the frame's HOP_HOT ops are the hit map a press is tested against
   (tagpu_wikistate.c). Pure C99 and the C library.

   Every coordinate is in wiki units from the screen's top left: the screen's height / 720 device
   pixels, whatever the HUD scale (the mockup's --u). */
#include "tagpu_hudlay.h"

typedef struct {
    float W, H;             /* the game screen, in units                                          */
    float bar;              /* the units a top-bar pixel is: the bar's height in units / 32      */
    int   rewind;           /* this frame draws the live replay HUD's rewind button               */
    int   open;             /* the window is open                                                 */
    int   btn_down;         /* the wiki button shows pressed (it also does while open)            */
    int   x_down;           /* the close button shows pressed                                     */
} WIKIFRAME;

/* The ops for `in`, into `out` (never on a stack), with the kit `kit` (tagpu_hudart.h, WIKIART:
   its parts and the HUD's styles): the button always, the window while open. The body's rectangle
   in `*body` (w = 0 while closed). Returns 0 with the reason in `err` when the kit lacks a part
   or a style it draws. The hot keys: "wiki", and while open "window" under "close". */
int tagpu_wikiframe_build(const HUDLAY* kit, const WIKIFRAME* in, HUDOPS* out, HUDRECT* body,
                          char* err, size_t errn);

#endif
