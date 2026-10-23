/* tagpu_hudtext.c -- one line of HUD text as the browser sets it. Contract: tagpu_hudtext.h. */
#include "tagpu_hudtext.h"

uint32_t tagpu_hudtext_utf8(const unsigned char** s, const unsigned char* e)
{
    const unsigned char* p = *s;
    uint32_t c = *p++;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (c >= 0x80 && n == 0) { *s = p; return 0xFFFD; }
    c &= n == 3 ? 0x07 : n == 2 ? 0x0F : n == 1 ? 0x1F : 0x7F;
    while (n-- > 0) {
        if (p >= e || (*p & 0xC0) != 0x80) { *s = p; return 0xFFFD; }
        c = (c << 6) | (*p++ & 0x3F);
    }
    *s = p;
    return c;
}

/* ß has no one-letter capital and stays; ÿ becomes Ÿ, outside Latin-1 */
uint32_t tagpu_hudtext_upper(uint32_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
    if (c == 0xFF) return 0x178;
    return c;
}

float tagpu_hudtext_pens(const stbtt_fontinfo* f, float sc, float sp, const int* gi, int n, float* pen)
{
    float width = 0;
    int i;
    for (i = 0; i < n; i++) {
        int adv, lsb;
        stbtt_GetGlyphHMetrics(f, gi[i], &adv, &lsb);
        if (i) width += sc * (float)stbtt_GetGlyphKernAdvance(f, gi[i - 1], gi[i]);
        if (pen) pen[i] = width;
        width += sc * (float)adv + sp;
    }
    return width;
}

float tagpu_hudtext_width(const stbtt_fontinfo* f, float size, float spacing, int upper,
                          const char* s, size_t len)
{
    const unsigned char *p = (const unsigned char*)s, *e = p + len;
    int gi[TAGPU_HUDTEXT_MAX], n = 0;
    while (p < e && n < TAGPU_HUDTEXT_MAX) {
        uint32_t ch = tagpu_hudtext_utf8(&p, e);
        gi[n++] = stbtt_FindGlyphIndex(f, (int)(upper ? tagpu_hudtext_upper(ch) : ch));
    }
    return tagpu_hudtext_pens(f, stbtt_ScaleForMappingEmToPixels(f, size), spacing * size, gi, n, NULL);
}
