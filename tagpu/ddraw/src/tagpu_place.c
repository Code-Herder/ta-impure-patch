/* tagpu_place.c -- several buildings in one gesture (tagpu_place.h). */
#include "tagpu_place.h"

void tagpu_place_init(void) {}

void tagpu_place_level_end(void) {}

unsigned tagpu_place_sites(TagpuPlaceSite* out, unsigned max)
{
    (void)out; (void)max;
    return 0;
}

unsigned tagpu_place_gdi_rects(const int engine[4], int out[][4], unsigned char* colour,
                               unsigned max)
{
    (void)engine; (void)out; (void)colour; (void)max;
    return 0;
}
