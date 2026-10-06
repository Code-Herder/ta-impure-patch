#ifndef TAGPU_PERSP_H
#define TAGPU_PERSP_H
/* tagpu_persp -- the playback's arithmetic on a recorded seat's camera and cursor
   (research/notes/tadr-port/demo-recorder-perspective-streams.md, *Playback*): the perspective
   clock, which picks or interpolates the sample a displayed GameTime shows, and Follow's fit of
   that sample to the viewer's own screen. Engine-free and thread-free: every input is a copied
   value, every output an integer, so the same answers come out of tools/replay/tarec.py's mirror
   (persp_sample, persp_follow) to the bit, which tools/replay/persp_test.py checks. A change here
   is a change there. */

#include <stdint.h>
#include "tagpu_recpov.h"

/* Which sample the clock answered with. */
enum {
    PERSP_NONE = 0,     /* the seat has no sample                                            */
    PERSP_LATEST,       /* the latest, taken at least its `every` before the displayed tick  */
    PERSP_BETWEEN,      /* interpolated between the two held samples                         */
    PERSP_EARLIER       /* the earlier one (or the only one): the latest is too new and the
                           two are too far apart, or too new, to interpolate between          */
};

/* THE PERSPECTIVE CLOCK for the displayed GameTime `n`, over a seat's `count` held samples
   (0, 1 or 2), `pov[count - 1]` the latest, `tick[i]` the GameTime each was taken -- RecWorld's
   npov/pov/pov_tick (tagpu_recreader.h). With b the latest and a the one before it, and
   t = n - b.every (the playback runs `every` ticks behind the world, so both samples around t are
   in hand without reading ahead):
     b when b.tick <= t;
     else, when a.tick <= t < b.tick and b.tick - a.tick <= 2 * b.every, every field of a moved
       toward b by (b - a)(t - a.tick)/(b.tick - a.tick), in 64-bit integers truncated toward
       zero -- the cursor only when both samples are REC_POV_WORLD, a's cursor and flags
       otherwise;
     else a, or b when it is the only one.
   `*out` gets the answer; returns a PERSP_* code, PERSP_NONE leaving `*out` alone. */
int persp_sample(unsigned count, const RecPov pov[2], const unsigned tick[2], unsigned n,
                 RecPov* out);

/* FOLLOW'S FIT (the owner's answer Q1): the recorder's visible area on the viewer's screen.
   The viewer's view `view_w` x `view_h` is the 1x view main+0x37E37/+0x37E3B, as the sample's own
   view_w/view_h are the recorder's. The view's centre is eye + view/2 at every zoom
   (tagpu_zoom.c, "the camera's range"), so the eye is the sample's camera less half the viewer's
   view. The level, in Q16, is
       z_v = clamp(z_r * min(view_w / rec_w, view_h / rec_h), REC_POV_ZOOM_MIN, REC_POV_ZOOM_MAX)
   with each product taken in 64 bits and truncated: rounding down keeps view/z_v at least the
   recorder's rec/z_r on both axes, so everything the recorder saw is on the viewer's screen
   unless the range's floor cut it. Returns 0, writing nothing, for a view of no size. */
int persp_follow(const RecPov* p, int view_w, int view_h, int* eye_x, int* eye_y,
                 uint32_t* level_q16);

#endif
