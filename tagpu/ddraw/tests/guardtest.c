/* guardtest.c -- tagpu_guardgeo.c's self-test (guardtest.py builds it natively with ASan and UBSan). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_guardgeo.h"

static int s_fail;
#define CHECK(c, ...) do { if (!(c)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                         printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { W = 48, H = 48 };
static uint8_t s_seen[W * H / 8 + 1], s_pass[W * H], s_fit[W * H], s_taken[W * H], s_taken2[W * H];
static int16_t s_qx[W * H], s_qz[W * H];
static int32_t s_qf[W * H], s_sums[(W + 1) * (H + 1)], s_bsums[(W + 1) * (H + 1)];
static const GgWork s_wk = { s_seen, s_qx, s_qz, s_qf, s_sums };

static void test_exits(void)
{
    /* a 4 x 4 plant: the bottom row walks off (0x04), the top row is building (0x00), the left column's middle a
       cell open only while the yard is (0x02 | 0x04) */
    uint8_t yard[16] = { 0, 0, 0, 0,  6, 0, 0, 0,  6, 0, 0, 0,  4, 4, 4, 4 };
    GgExits L;
    gg_exits(yard, 4, 4, &L);
    /* the bottom row's corners open the left and right edges too */
    CHECK(L.n == 3, "exits %d", L.n);
    CHECK(L.edge[0] == 1 && L.a[0] == 0 && L.e[0] == 3, "bottom %d %d %d", L.edge[0], L.a[0], L.e[0]);
    CHECK(L.edge[1] == 2 && L.a[1] == 3 && L.e[1] == 3, "left %d %d %d", L.edge[1], L.a[1], L.e[1]);
    CHECK(L.edge[2] == 3 && L.a[2] == 3 && L.e[2] == 3, "right %d %d %d", L.edge[2], L.a[2], L.e[2]);
}

static void test_band(void)
{
    CHECK(gg_rin(2, 2) == 22, "rin 2x2 %d", gg_rin(2, 2));   /* (int)(16 sqrt 8) = 45, /2 */
    CHECK(gg_rin(3, 3) == 30, "rin 3x3 %d", gg_rin(3, 3));   /* (int)(16 sqrt 15) = 61 */
    CHECK(gg_goal_place(100 << 16, 2) == 5, "goal %d", gg_goal_place(100 << 16, 2));
    CHECK(gg_goal_place(-(100 << 16), 2) == -7, "goal neg %d", gg_goal_place(-(100 << 16), 2));
}

/* a 6 x 6 plant at (21, 21) in the 48 x 48 box from (0, 0), the exit the bottom row */
static void plant_box(GgBox* box, GgRect* plant, GgExits* L)
{
    uint8_t yard[36];
    box->x0 = 0; box->z0 = 0; box->w = W; box->h = H;
    plant->x0 = 21; plant->z0 = 21; plant->x1 = 26; plant->z1 = 26;
    memset(yard, 0, sizeof yard);
    memset(yard + 30, 4, 6);
    gg_exits(yard, 6, 6, L);
}

static void test_lane(void)
{
    GgBox box;
    GgRect plant, out[16];
    GgExits L;
    uint8_t blocked[W * H];
    int n = 0, i;
    plant_box(&box, &plant, &L);
    memset(blocked, 0, sizeof blocked);
    for (i = 21; i <= 26; i++) memset(blocked + i * W + 21, 1, 6);
    gg_sums(blocked, W, H, s_bsums);
    memset(s_fit, 1, sizeof s_fit);
    gg_square_fit(s_fit, W, H, 0, 0);
    CHECK(gg_lane(&box, &plant, &L, -1, 3, 8, 1, s_fit, s_bsums, &s_wk, out, &n, 16) == 1, "open lane");
    CHECK(n >= 1, "lane rects %d", n);
    for (i = 0; i < n; i++)
        CHECK(out[i].z0 >= 26, "lane %d runs below the plant: %d %d %d %d", i, out[i].x0, out[i].z0, out[i].x1,
              out[i].z1);
    /* a wall right under the exit: the lane turns aside */
    memset(blocked + 27 * W + 15, 1, 18);
    gg_sums(blocked, W, H, s_bsums);
    n = 0;
    CHECK(gg_lane(&box, &plant, &L, -1, 3, 8, 1, s_fit, s_bsums, &s_wk, out, &n, 16) == 0,
          "a wall across the only exit's mouth leaves no lane");
    memset(blocked + 27 * W + 15, 0, 18);
    memset(blocked + 30 * W + 18, 1, 12);
    gg_sums(blocked, W, H, s_bsums);
    n = 0;
    CHECK(gg_lane(&box, &plant, &L, -1, 3, 8, 1, s_fit, s_bsums, &s_wk, out, &n, 16) == 1, "a wall further out");
}

static int in_band(int x, int z, int gx, int gz, int r2min, int r2max)
{
    const int d2 = (x - gx) * (x - gx) + (z - gz) * (z - gz);
    return d2 >= r2min && d2 <= r2max;
}

static void test_seats(void)
{
    GgBox box;
    GgRect plant, lane[16];
    GgExits L;
    GgSeated seated[64];
    uint8_t blocked[W * H];
    int nl = 0, ns = 0, i, k, x, z;
    const int gx = 23, gz = 23, r2min = 4, r2max = 81;   /* a 2 x 2 helper, the frame at the plant's middle */
    plant_box(&box, &plant, &L);
    memset(blocked, 0, sizeof blocked);
    for (i = 21; i <= 26; i++) memset(blocked + i * W + 21, 1, 6);
    gg_sums(blocked, W, H, s_bsums);
    memset(s_fit, 1, sizeof s_fit);
    CHECK(gg_lane(&box, &plant, &L, -1, 3, 12, 1, s_fit, s_bsums, &s_wk, lane, &nl, 16) == 1, "lane");
    memset(s_pass, 1, sizeof s_pass);
    memcpy(s_taken, blocked, sizeof s_taken);
    for (k = 0; k < nl; k++)
        for (z = lane[k].z0; z <= lane[k].z1; z++)
            for (x = lane[k].x0; x <= lane[k].x1; x++) s_taken[z * W + x] = 1;
    while (ns < 64) {
        int sx, sz;
        if (!gg_seat(&box, 2, 2, s_pass, s_taken, gx, gz, r2min, r2max, seated, ns, 400, &s_wk, s_taken2, &sx, &sz))
            break;
        CHECK(in_band(sx, sz, gx, gz, r2min, r2max), "seat %d out of the band", ns);
        for (k = 0; k < nl; k++) CHECK(!gg_overlaps(&lane[k], sx, sz, 2, 2), "seat %d on the lane", ns);
        seated[ns].x = sx; seated[ns].z = sz; seated[ns].fx = 2; seated[ns].fz = 2; seated[ns].pass = s_pass;
        s_taken[sz * W + sx] = s_taken[sz * W + sx + 1] = s_taken[(sz + 1) * W + sx] = s_taken[(sz + 1) * W + sx + 1] = 2;
        ns++;
    }
    CHECK(ns >= 12, "only %d seats around a 6 x 6 plant", ns);
    for (k = 0; k < ns; k++)
        CHECK(gg_way_out(&box, seated[k].x, seated[k].z, 2, 2, s_pass, s_taken, &s_wk), "seat %d boxed in", k);
    printf("guardtest: %d seats, %d lane rectangles\n", ns, nl);
}

static void test_wait(void)
{
    GgBox box = { 0, 0, W, H };
    int wx, wz;
    memset(s_pass, 1, sizeof s_pass);
    memset(s_taken, 0, sizeof s_taken);
    CHECK(gg_wait_spot(&box, 10, 10, 2, 2, s_pass, s_taken, 4, &wx, &wz) && wx == 10 && wz == 10, "free spot kept");
    s_taken[10 * W + 10] = 1;
    CHECK(gg_wait_spot(&box, 10, 10, 2, 2, s_pass, s_taken, 4, &wx, &wz) && !(wx == 10 && wz == 10) &&
          abs(wx - 10) <= 1 && abs(wz - 10) <= 1, "moved one ring: %d %d", wx, wz);
}

int main(void)
{
    test_exits();
    test_band();
    test_lane();
    test_seats();
    test_wait();
    printf("guardtest: %s\n", s_fail ? "FAILED" : "ok");
    return s_fail != 0;
}
