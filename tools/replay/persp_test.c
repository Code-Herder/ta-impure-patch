/* The playback's perspective arithmetic (tagpu/ddraw/src/tagpu_persp.c), compiled as it is and
   run natively by persp_test.py. With no argument: the checks below, each a rule of the plan's
   *Playback* section. With `cases`: one line of answers per line of cases on stdin, which the
   driver compares with tarec.py's mirror. */
#include "tagpu_persp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %u: %s\n", __LINE__, #x); exit(1); } } while (0)

static RecPov pov(unsigned every, unsigned flags, int cam_x, int cam_y, unsigned zoom)
{
    RecPov p;
    memset(&p, 0, sizeof p);
    p.every = (uint8_t)every;
    p.flags = (uint8_t)flags;
    p.view_w = 640;
    p.view_h = 480;
    p.zoom = zoom;
    p.cam_x = cam_x;
    p.cam_y = cam_y;
    p.cur_x = cam_x;
    p.cur_z = cam_y;
    return p;
}

static void checks(void)
{
    RecPov two[2], out;
    unsigned tick[2] = { 10, 13 };
    int x, y;
    uint32_t z;

    two[0] = pov(3, REC_POV_WORLD, 100, 200, 65536);
    two[1] = pov(3, REC_POV_WORLD, 130, 170, 131072);
    CHECK(persp_sample(0, two, tick, 50, &out) == PERSP_NONE);
    /* the latest once it is `every` old */
    CHECK(persp_sample(2, two, tick, 16, &out) == PERSP_LATEST && out.cam_x == 130);
    /* between: t = 14 - 3 = 11, a third of the way */
    CHECK(persp_sample(2, two, tick, 14, &out) == PERSP_BETWEEN);
    CHECK(out.cam_x == 110 && out.cam_y == 190 && out.zoom == 65536 + 65536 / 3);
    CHECK(out.cur_x == 110 && out.cur_z == 190);
    /* truncated toward zero: -30/3 * 2 = -20 exactly, -40 * 1/3 = -13.3 -> -13 */
    two[1].cam_y = 160;
    CHECK(persp_sample(2, two, tick, 14, &out) == PERSP_BETWEEN && out.cam_y == 187);
    /* t before the earlier sample: the earlier one */
    CHECK(persp_sample(2, two, tick, 12, &out) == PERSP_EARLIER && out.cam_x == 100);
    /* two samples further apart than 2 * every do not interpolate */
    tick[1] = 17;
    CHECK(persp_sample(2, two, tick, 18, &out) == PERSP_EARLIER && out.cam_x == 100);
    tick[1] = 13;
    /* the cursor only between two world samples: a's otherwise, and a's flags */
    two[1].flags = REC_POV_MINIMAP;
    two[1].cur_x = 999;
    CHECK(persp_sample(2, two, tick, 14, &out) == PERSP_BETWEEN && out.cam_x == 110 &&
          out.cur_x == 100 && out.flags == REC_POV_WORLD);
    /* one sample: it, whatever its age */
    CHECK(persp_sample(1, two, tick, 0, &out) == PERSP_EARLIER && out.cam_x == 100);
    CHECK(persp_sample(1, two, tick, 13, &out) == PERSP_LATEST);

    /* Follow's fit: the same screen keeps the level and centres the camera */
    two[0] = pov(3, REC_POV_WORLD, 1000, 800, 65536);
    CHECK(persp_follow(&two[0], 640, 480, &x, &y, &z) && x == 680 && y == 560 && z == 65536);
    /* a wider screen: the tighter axis (480 -> 600 is x1.25, 640 -> 1000 x1.5625) decides */
    CHECK(persp_follow(&two[0], 1000, 600, &x, &y, &z) && x == 500 && y == 500 && z == 81920);
    /* truncated, so the recorder's area is never cut: 65536 * 700 / 640 = 71680 exactly,
       65536 * 500 / 480 = 68266.67 -> 68266 */
    CHECK(persp_follow(&two[0], 700, 500, &x, &y, &z) && z == 68266);
    /* the range */
    two[0].zoom = 524288;
    CHECK(persp_follow(&two[0], 3840, 2160, &x, &y, &z) && z == REC_POV_ZOOM_MAX);
    two[0].zoom = 16384;
    CHECK(persp_follow(&two[0], 320, 200, &x, &y, &z) && z == REC_POV_ZOOM_MIN);
    /* an odd view: the eye is the camera less half of it, rounded down */
    CHECK(persp_follow(&two[0], 641, 481, &x, &y, &z) && x == 680 && y == 560);
    /* no view, no answer */
    CHECK(!persp_follow(&two[0], 0, 480, &x, &y, &z) && !persp_follow(&two[0], 640, -1, &x, &y, &z));
    /* a camera at the end of int32 saturates */
    two[0].cam_x = INT32_MIN;
    CHECK(persp_follow(&two[0], 640, 480, &x, &y, &z) && x == INT32_MIN);
    printf("persp: every check holds\n");
}

static int read_pov(RecPov* p)
{
    unsigned every, flags, vw, vh, zoom;
    int cx, cy, ux, uh, uz;
    if (scanf("%u %u %u %u %u %d %d %d %d %d", &every, &flags, &vw, &vh, &zoom, &cx, &cy, &ux,
              &uh, &uz) != 10)
        return 0;
    memset(p, 0, sizeof *p);
    p->every = (uint8_t)every;
    p->flags = (uint8_t)flags;
    p->view_w = (uint16_t)vw;
    p->view_h = (uint16_t)vh;
    p->zoom = zoom;
    p->cam_x = cx; p->cam_y = cy;
    p->cur_x = ux; p->cur_h = uh; p->cur_z = uz;
    return 1;
}

/* count t0 t1 n view_w view_h, then two samples: every flags view_w view_h zoom cam_x cam_y
   cur_x cur_h cur_z. Answers: how, the sample's fields, then the fit (ok, eye x, eye y, level). */
static void cases(void)
{
    unsigned count, tick[2], n;
    int vw, vh;
    while (scanf("%u %u %u %u %d %d", &count, &tick[0], &tick[1], &n, &vw, &vh) == 6) {
        RecPov two[2], out;
        int how, ok, x = 0, y = 0;
        uint32_t z = 0;
        if (!read_pov(&two[0]) || !read_pov(&two[1])) break;
        memset(&out, 0, sizeof out);
        how = persp_sample(count, two, tick, n, &out);
        ok = how && persp_follow(&out, vw, vh, &x, &y, &z);
        printf("%d %u %u %u %u %u %d %d %d %d %d %d %d %d %u\n", how, out.every, out.flags,
               out.view_w, out.view_h, (unsigned)out.zoom, out.cam_x, out.cam_y, out.cur_x,
               out.cur_h, out.cur_z, ok, x, y, (unsigned)z);
    }
}

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "cases")) cases();
    else checks();
    return 0;
}
