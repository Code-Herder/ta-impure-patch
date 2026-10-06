/* tagpu_persp.c -- the perspective clock and Follow's fit (tagpu_persp.h). No engine memory, no
   Win32, no floating point: tools/replay/tarec.py mirrors each line, and the playback's eye is
   compared with the mirror's to the world pixel. */
#include "tagpu_persp.h"

/* a + (b - a)(num / den), truncated toward zero as C99's division is; den > 0, 0 <= num < den.
   |b - a| < 2^33 and num < 2 * REC_POV_EVERY_MAX, so the product fits 64 bits. */
static int64_t lerp(int64_t a, int64_t b, int64_t num, int64_t den)
{
    return a + (b - a) * num / den;
}

/* A camera is data: one recorded near either end of int32 saturates rather than wrapping, and
   the zoom's range clamp then bounds the eye like any other. */
static int32_t sat32(int64_t v)
{
    return v < INT32_MIN ? INT32_MIN : v > INT32_MAX ? INT32_MAX : (int32_t)v;
}

int persp_sample(unsigned count, const RecPov pov[2], const unsigned tick[2], unsigned n,
                 RecPov* out)
{
    const RecPov *a, *b;
    int64_t t, ta, tb;
    if (!count || count > 2) return PERSP_NONE;
    b = &pov[count - 1];
    tb = tick[count - 1];
    t = (int64_t)n - b->every;
    if (tb <= t) {
        *out = *b;
        return PERSP_LATEST;
    }
    if (count == 1) {
        *out = *b;
        return PERSP_EARLIER;
    }
    a = &pov[0];
    ta = tick[0];
    *out = *a;
    if (ta <= t && tb - ta <= 2 * (int64_t)b->every) {
        int64_t num = t - ta, den = tb - ta;
        out->view_w = (uint16_t)lerp(a->view_w, b->view_w, num, den);
        out->view_h = (uint16_t)lerp(a->view_h, b->view_h, num, den);
        out->zoom = (uint32_t)lerp(a->zoom, b->zoom, num, den);
        out->cam_x = (int32_t)lerp(a->cam_x, b->cam_x, num, den);
        out->cam_y = (int32_t)lerp(a->cam_y, b->cam_y, num, den);
        if ((a->flags & REC_POV_WORLD) && (b->flags & REC_POV_WORLD)) {
            out->cur_x = (int32_t)lerp(a->cur_x, b->cur_x, num, den);
            out->cur_h = (int32_t)lerp(a->cur_h, b->cur_h, num, den);
            out->cur_z = (int32_t)lerp(a->cur_z, b->cur_z, num, den);
        }
        return PERSP_BETWEEN;
    }
    return PERSP_EARLIER;
}

int persp_follow(const RecPov* p, int view_w, int view_h, int* eye_x, int* eye_y,
                 uint32_t* level_q16)
{
    int64_t zx, zy, z;
    if (view_w <= 0 || view_h <= 0 || !p->view_w || !p->view_h) return 0;
    zx = (int64_t)p->zoom * view_w / p->view_w;
    zy = (int64_t)p->zoom * view_h / p->view_h;
    z = zx < zy ? zx : zy;
    if (z < REC_POV_ZOOM_MIN) z = REC_POV_ZOOM_MIN;
    if (z > REC_POV_ZOOM_MAX) z = REC_POV_ZOOM_MAX;
    *eye_x = sat32((int64_t)p->cam_x - view_w / 2);
    *eye_y = sat32((int64_t)p->cam_y - view_h / 2);
    *level_q16 = (uint32_t)z;
    return 1;
}
