/* iconsettest.c -- a mod icon set's pure half (src/tagpu_iconset.c), run on
   the host by tests/iconsettest.py under the sanitizers, or under Wine as
   `make iconsettest`.

     1. the PCX decoder on synthetic files: runs that cross a row, a stride
        wider than the image, a run past the image's end, a zero-length run;
        refused -- a short header, a truncated stream, a run byte with no
        value, 23 px, a stride under the width or past the limit, a reversed
        extent, the wrong bits, planes or encoding, an output too small --
        and a mutation fuzz that must never write out of bounds or report a
        size out of range;
     2. the profile reader: comments, a `;` in a value, CRLF, a section
        header with a comment after it, a repeated section (the first is
        read), keys in any case, a key with no `=`, a value too long for its
        buffer (refused, never cut), TADR's booleans and colours;
     3. the iconcfg in both modes, its defaults with every key missing, and the
        load list each gives -- the sides first, then the fixed list or the
        [Icon] lines with their three special keys;
     4. the names: the TADR ini's file name, a relative path, a side's name;
     5. the atlas: the texel classes' precedence, the three markers chosen
        over every picture, the worst case -- 16 logo frames of 16 px and 64
        set pictures of 22 px -- fitting with every rect inside and none
        overlapping, and refusal when every index is drawn.

   No mod's file is in this test or beside it: every input is built here.
   Exit 0 when nothing failed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_iconset.h"

static int s_fail, s_checks;

#define CHECK(c, ...) do { s_checks++; if (!(c)) { s_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- 1. PCX ---- */

/* A PCX header for a w x h image with stride bpl, extent starting at (x0, y0). */
static size_t pcx_header(unsigned char* b, int w, int h, int bpl, int x0, int y0)
{
    memset(b, 0, 128);
    b[0] = 0x0A; b[1] = 5; b[2] = 1; b[3] = 8;
    b[4] = (unsigned char)x0; b[5] = (unsigned char)(x0 >> 8);
    b[6] = (unsigned char)y0; b[7] = (unsigned char)(y0 >> 8);
    b[8] = (unsigned char)(x0 + w - 1); b[9] = (unsigned char)((x0 + w - 1) >> 8);
    b[10] = (unsigned char)(y0 + h - 1); b[11] = (unsigned char)((y0 + h - 1) >> 8);
    b[65] = 1;
    b[66] = (unsigned char)bpl; b[67] = (unsigned char)(bpl >> 8);
    return 128;
}

/* RLE of `len` bytes as ONE stream, so runs cross rows; every value at or
   over 0xC0 is a run of at least one, as the format requires */
static size_t rle(unsigned char* o, const unsigned char* s, size_t len)
{
    size_t i = 0, k = 0;
    while (i < len) {
        size_t r = 1;
        while (i + r < len && s[i + r] == s[i] && r < 63) r++;
        if (r > 1 || s[i] >= 0xC0) { o[k++] = (unsigned char)(0xC0 | r); o[k++] = s[i]; }
        else o[k++] = s[i];
        i += r;
    }
    return k;
}

/* a whole file: the w x h image `img` padded to `bpl` with 0xEE, then a
   256-colour palette the decoder must ignore */
static size_t pcx_make(unsigned char* f, const unsigned char* img, int w, int h, int bpl)
{
    unsigned char raw[4096];
    size_t n = pcx_header(f, w, h, bpl, 3, 7);
    int r;
    for (r = 0; r < h; r++) {
        memcpy(raw + r * bpl, img + r * w, (size_t)w);
        memset(raw + r * bpl + w, 0xEE, (size_t)(bpl - w));
    }
    n += rle(f + n, raw, (size_t)(h * bpl));
    f[n++] = 0x0C;
    memset(f + n, 0x55, 768);
    return n + 768;
}

static void test_pcx(void)
{
    unsigned char img[22 * 22], f[8192], out[22 * 22];
    int w, h, i, ok;
    size_t n;

    /* a 10 x 10 icon of long runs that cross rows, the shape every set ships */
    for (i = 0; i < 100; i++) img[i] = (unsigned char)(i < 35 ? 9 : i < 70 ? 245 : (i * 7) & 0xFF);
    n = pcx_make(f, img, 10, 10, 10);
    ok = tagpu_pcx_decode(f, n, out, sizeof out, &w, &h);
    CHECK(ok && w == 10 && h == 10 && !memcmp(out, img, 100), "10x10 with runs across rows");

    /* a stride wider than the image: the padding is not kept */
    for (i = 0; i < 7 * 5; i++) img[i] = (unsigned char)(0xC0 + i);
    n = pcx_make(f, img, 7, 5, 8);
    ok = tagpu_pcx_decode(f, n, out, sizeof out, &w, &h);
    CHECK(ok && w == 7 && h == 5 && !memcmp(out, img, 35), "7x5 at stride 8, values >= 0xC0");

    /* 22 x 22 is the largest; 23 on either side is refused */
    memset(img, 3, sizeof img);
    n = pcx_make(f, img, 22, 22, 22);
    CHECK(tagpu_pcx_decode(f, n, out, sizeof out, &w, &h) && w == 22 && h == 22, "22x22 decodes");
    pcx_header(f, 23, 4, 24, 0, 0);
    CHECK(!tagpu_pcx_decode(f, n, out, sizeof out, &w, &h) && w == 0 && h == 0, "23 wide is refused");
    pcx_header(f, 4, 23, 4, 0, 0);
    CHECK(!tagpu_pcx_decode(f, n, out, sizeof out, &w, &h), "23 tall is refused");

    /* the output too small for the image */
    n = pcx_make(f, img, 10, 10, 10);
    CHECK(!tagpu_pcx_decode(f, n, out, 99, &w, &h), "an output of 99 bytes for 100 texels is refused");
    CHECK(tagpu_pcx_decode(f, n, out, 100, &w, &h), "an output of exactly 100 bytes is enough");

    /* the header's fields */
    n = pcx_make(f, img, 10, 10, 10);
    f[3] = 4;
    CHECK(!tagpu_pcx_decode(f, n, out, sizeof out, &w, &h), "4 bits a pixel is refused");
    f[3] = 8; f[65] = 3;
    CHECK(!tagpu_pcx_decode(f, n, out, sizeof out, &w, &h), "three planes are refused");
    f[65] = 1; f[2] = 0;
    CHECK(!tagpu_pcx_decode(f, n, out, sizeof out, &w, &h), "an unencoded file is refused");
    f[2] = 1; f[0] = 0x0B;
    CHECK(!tagpu_pcx_decode(f, n, out, sizeof out, &w, &h), "a wrong manufacturer byte is refused");
    f[0] = 0x0A; f[66] = 9;
    CHECK(!tagpu_pcx_decode(f, n, out, sizeof out, &w, &h), "a stride under the width is refused");
    f[66] = 0x01; f[67] = 0x04;                              /* 1025 */
    CHECK(!tagpu_pcx_decode(f, n, out, sizeof out, &w, &h), "a stride past 1024 is refused");
    pcx_header(f, 10, 10, 10, 50, 50);
    f[8] = 10;                                               /* xmax < xmin */
    CHECK(!tagpu_pcx_decode(f, n, out, sizeof out, &w, &h), "a reversed extent is refused");
    CHECK(!tagpu_pcx_decode(f, 127, out, sizeof out, &w, &h), "a short header is refused");
    CHECK(!tagpu_pcx_decode(NULL, 0, out, sizeof out, &w, &h), "no bytes is refused");

    /* a truncated stream, at every length short of the last data byte */
    for (i = 0; i < 100; i++) img[i] = (unsigned char)(i * 37);
    n = pcx_make(f, img, 10, 10, 10) - 769;                  /* the image's own bytes */
    {
        size_t cut;
        int refusedAll = 1;
        for (cut = 128; cut < n; cut++)
            refusedAll &= !tagpu_pcx_decode(f, cut, out, sizeof out, &w, &h);
        CHECK(refusedAll, "every truncation of the stream is refused");
        CHECK(tagpu_pcx_decode(f, n, out, sizeof out, &w, &h), "the untruncated stream decodes");
    }

    /* a run marker as the stream's last byte: no value follows */
    pcx_header(f, 2, 1, 2, 0, 0);
    f[128] = 0xC2;
    CHECK(!tagpu_pcx_decode(f, 129, out, sizeof out, &w, &h), "a run byte with no value is refused");

    /* a run past the image's last byte ends with it; a zero-length run is nothing */
    pcx_header(f, 4, 2, 4, 0, 0);
    f[128] = 0xC0; f[129] = 0x77;                            /* zero-length */
    f[130] = 0xFF; f[131] = 0x21;                            /* 63 of 0x21 for 8 texels */
    memset(out, 0, sizeof out);
    ok = tagpu_pcx_decode(f, 132, out, 8, &w, &h);
    CHECK(ok && w == 4 && h == 2, "a run overshooting the image decodes the image");
    for (i = 0; i < 8; i++) CHECK(out[i] == 0x21, "overshoot texel %d is 0x%02X", i, out[i]);

    /* mutations of a valid file: whatever the bytes say, nothing is written
       past `cap` (the sanitizers watch a buffer exactly that long) and any
       success reports a size in range */
    {
        unsigned seed = 12345u;
        int t, good = 0;
        unsigned char base[2048];
        size_t bn;
        for (i = 0; i < 16 * 16; i++) img[i] = (unsigned char)((i * 13) ^ (i >> 3));
        bn = pcx_make(base, img, 16, 16, 18);
        for (t = 0; t < 20000; t++) {
            unsigned char* m = (unsigned char*)malloc(bn);
            unsigned char* o;
            int flips = 1 + (int)((seed >> 8) % 6), cap, k;
            size_t len = bn;
            memcpy(m, base, bn);
            for (k = 0; k < flips; k++) {
                seed = seed * 1103515245u + 12345u;
                m[(seed >> 4) % bn] = (unsigned char)(seed >> 20);
            }
            seed = seed * 1103515245u + 12345u;
            if (seed & 1) len = (seed >> 3) % bn;
            cap = 1 + (int)((seed >> 12) % (22 * 22));
            o = (unsigned char*)malloc((size_t)cap);
            if (tagpu_pcx_decode(m, len, o, cap, &w, &h)) {
                good++;
                CHECK(w >= 1 && h >= 1 && w <= 22 && h <= 22 && w * h <= cap, "fuzz %d: %dx%d cap %d", t, w,
                      h, cap);
            }
            free(o);
            free(m);
        }
        CHECK(good > 0, "the fuzz decoded none of its mutations");
    }
}

/* ---- 2. the profile reader ---- */
static const char* const INI =
    "; a comment at the top\r\n"
    "[Preferences] ; trailing words after the name\r\n"
    "  MegaMapConfig = .\\Icon\\iconcfg.ini;   ; the set\r\n"
    "Spaced Key   =   spaced value   \r\n"
    "NoEquals\r\n"
    "Empty=\r\n"
    "Long=0123456789012345678901234567890123456789\r\n"
    "mixedCASE=yes\r\n"
    "Dup=first\r\n"
    "Dup=second\r\n"
    "[ Other ]\n"
    "Key=other\n"
    "[preferences]\n"
    "Later=never read\n"
    "MegaMapConfig=never read either\n";

typedef struct { int n; char key[8][64], val[8][260]; } LINES;
static int keep_line(const char* k, const char* v, void* ctx)
{
    LINES* l = (LINES*)ctx;
    if (l->n < 8) { strcpy(l->key[l->n], k); strcpy(l->val[l->n], v); }
    l->n++;
    return 1;
}
static int stop_after_two(const char* k, const char* v, void* ctx)
{
    (void)k; (void)v;
    return ++*(int*)ctx < 2;
}

static void test_ini(void)
{
    char v[64], tiny[8];
    const size_t n = strlen(INI);
    LINES l;
    int refused, count;

    CHECK(tagpu_ini_get(INI, n, "Preferences", "MegaMapConfig", v, sizeof v) == 1 &&
          !strcmp(v, ".\\Icon\\iconcfg.ini"), "MegaMapConfig = '%s'", v);
    CHECK(tagpu_ini_get(INI, n, "PREFERENCES", "megamapconfig", v, sizeof v) == 1 &&
          !strcmp(v, ".\\Icon\\iconcfg.ini"), "the names fold case");
    CHECK(tagpu_ini_get(INI, n, "Preferences", "Spaced Key", v, sizeof v) == 1 && !strcmp(v, "spaced value"),
          "trimmed: '%s'", v);
    CHECK(tagpu_ini_get(INI, n, "Preferences", "MixedCase", v, sizeof v) == 1 && !strcmp(v, "yes"),
          "a key in another case");
    CHECK(tagpu_ini_get(INI, n, "Preferences", "Empty", v, sizeof v) == 1 && v[0] == 0, "an empty value");
    CHECK(tagpu_ini_get(INI, n, "Preferences", "NoEquals", v, sizeof v) == 0, "a line with no = is no key");
    CHECK(tagpu_ini_get(INI, n, "Preferences", "Dup", v, sizeof v) == 1 && !strcmp(v, "first"),
          "the first of a repeated key");
    CHECK(tagpu_ini_get(INI, n, "Preferences", "Later", v, sizeof v) == 0,
          "a repeated section is not read: the first one is");
    CHECK(tagpu_ini_get(INI, n, "Other", "Key", v, sizeof v) == 1 && !strcmp(v, "other"),
          "a section name with blanks inside its brackets");
    CHECK(tagpu_ini_get(INI, n, "Other", "MegaMapConfig", v, sizeof v) == 0, "keys belong to their section");
    CHECK(tagpu_ini_get(INI, n, "Missing", "Key", v, sizeof v) == 0, "a missing section");
    strcpy(tiny, "junk");
    CHECK(tagpu_ini_get(INI, n, "Preferences", "Long", tiny, sizeof tiny) == -1 && tiny[0] == 0,
          "a value longer than its buffer is refused, not cut");
    CHECK(tagpu_ini_get(INI, n, "Preferences", "Dup", tiny, 6) == 1 && !strcmp(tiny, "first"),
          "a value that exactly fits");
    CHECK(tagpu_ini_get(INI, 40, "Preferences", "Spaced Key", v, sizeof v) == 0,
          "the length bounds the read, not a NUL");

    memset(&l, 0, sizeof l);
    count = tagpu_ini_section(INI, n, "preferences", keep_line, &l, &refused);
    CHECK(count == 7 && l.n == 7 && refused == 0, "section lines %d, refused %d", count, refused);
    CHECK(!strcmp(l.key[0], "MegaMapConfig") && !strcmp(l.key[1], "Spaced Key") && !strcmp(l.key[6], "Dup") &&
          !strcmp(l.val[6], "second"), "the section's lines, in file order");
    {
        int seen = 0;
        CHECK(tagpu_ini_section(INI, n, "Preferences", stop_after_two, &seen, NULL) == 2 && seen == 2,
              "the callback stops the walk");
    }
    {
        /* a key past TAGPU_INI_KEYMAX is not passed, and is counted */
        char big[512];
        memset(&l, 0, sizeof l);
        strcpy(big, "[Icon]\n");
        memset(big + 7, 'K', 70);
        strcpy(big + 77, "=x.pcx\nok=y.pcx\n");
        count = tagpu_ini_section(big, strlen(big), "Icon", keep_line, &l, &refused);
        CHECK(count == 1 && refused == 1 && !strcmp(l.key[0], "ok"), "an oversized key is refused");
    }
    CHECK(tagpu_ini_get(NULL, 0, "Icon", "x", v, sizeof v) == 0, "no text");

    CHECK(tagpu_ini_bool("TRUE") && tagpu_ini_bool("true;") && tagpu_ini_bool("  It is True") &&
          !tagpu_ini_bool("FALSE") && !tagpu_ini_bool("") && !tagpu_ini_bool("tru"), "booleans");
    CHECK(tagpu_ini_byte("245", 7) == 245 && tagpu_ini_byte("0", 7) == 0 && tagpu_ini_byte("255x", 7) == 255,
          "colours");
    CHECK(tagpu_ini_byte("256", 7) == 7 && tagpu_ini_byte("abc", 7) == 7 && tagpu_ini_byte("", 7) == 7 &&
          tagpu_ini_byte("-3", 7) == 7 && tagpu_ini_byte("99999999999", 7) == 7, "colours out of range");
}

/* ---- 3. the iconcfg and its load list ---- */
static const char* const CFG_DEFAULT =
    "; a synthetic default-mode set\n"
    "[Option] ; colours\n"
    "FillColor=200;     ; the team colour\n"
    "TransparentColor=4;\n"
    "SelectedColor=250;\n"
    "UseDefaultIcon=TRUE; ; the fixed list\n"
    "[ICON]\n"
    "Unknow=U.PCX;\n";

static const char* const CFG_CUSTOM =
    "[option]\r\n"
    "fillcolor = 33\r\n"
    "UseDefaultIcon = False\r\n"
    "[Icon] ; the custom list\r\n"
    "NOTHING = none.pcx ;\r\n"
    ";TANK = tank.pcx\r\n"
    "Tank = TANK.PCX\r\n"
    "unknow=unk.pcx\r\n"
    "Plane=sub\\plane.pcx\r\n"
    "Bad=C:\\abs.pcx\r\n"
    "Rooted=\\abs.pcx\r\n"
    "NukeIcon=nuke.pcx\r\n"
    "Blank=\r\n"
    "=nokey.pcx\r\n"
    "[Icon]\r\n"
    "Second=never.pcx\r\n";

static void test_cfg(void)
{
    static TAGPU_ICONCFG c;
    static TAGPU_ISENTRY e[TAGPU_ICONSET_PLAN];
    const char* sides[5] = { "ARM", "CORE", "bad:side", NULL, "GOK" };
    int k, i;

    tagpu_iconcfg_parse(CFG_DEFAULT, strlen(CFG_DEFAULT), &c);
    CHECK(c.fill == 200 && c.transparent == 4 && c.selected == 250 && !c.custom && c.n == 1,
          "default mode: fill %d trans %d sel %d custom %d lines %d", c.fill, c.transparent, c.selected, c.custom,
          c.n);
    k = tagpu_iconset_plan(&c, sides, 5, e, TAGPU_ICONSET_PLAN);
    CHECK(k == 3 + 10, "default mode plan has %d entries", k);
    CHECK(e[0].kind == TAGPU_ISE_SIDE && e[0].arg == 0 && !strcmp(e[0].file, "ARM.PCX") &&
          !strcmp(e[0].name, "ARM"), "side 0 first");
    CHECK(e[1].kind == TAGPU_ISE_SIDE && e[1].arg == 1 && e[2].kind == TAGPU_ISE_SIDE && e[2].arg == 4 &&
          !strcmp(e[2].file, "GOK.PCX"), "an unusable side name is left out");
    {
        static const char* const want[10] = { "COMMANDER.PCX", "AIRCRAFTCOMBAT.PCX", "AIRCONS.PCX", "CONS.PCX",
            "MOBILECOMBAT.PCX", "FACTORY.PCX", "BUILDING.PCX", "NONE.PCX", "UNKNOWN.PCX", "NUKEICON.PCX" };
        for (i = 0; i < 10; i++) CHECK(!strcmp(e[3 + i].file, want[i]), "default %d is %s", i, e[3 + i].file);
        for (i = 0; i < TAGPU_ISD_N; i++)
            CHECK(e[3 + i].kind == TAGPU_ISE_DEFAULT && e[3 + i].arg == i, "default class %d", i);
        CHECK(e[10].kind == TAGPU_ISE_NONE && e[11].kind == TAGPU_ISE_UNKNOWN && e[12].kind == TAGPU_ISE_NUKE,
              "the three specials close the fixed list");
    }
    CHECK(tagpu_iconset_plan(&c, sides, 5, e, 4) == 4, "the plan stops at its cap");

    tagpu_iconcfg_parse(CFG_CUSTOM, strlen(CFG_CUSTOM), &c);
    CHECK(c.fill == 33 && c.transparent == 9 && c.selected == 89 && c.custom,
          "custom mode: fill %d trans %d sel %d custom %d", c.fill, c.transparent, c.selected, c.custom);
    CHECK(c.n == 5 && c.refused == 4, "custom lines %d refused %d", c.n, c.refused);
    k = tagpu_iconset_plan(&c, sides, 2, e, TAGPU_ICONSET_PLAN);
    CHECK(k == 2 + 5, "custom plan has %d entries", k);
    CHECK(e[2].kind == TAGPU_ISE_NONE && !strcmp(e[2].file, "none.pcx"), "NOTHING is NONE");
    CHECK(e[3].kind == TAGPU_ISE_CATEGORY && !strcmp(e[3].name, "Tank") && !strcmp(e[3].file, "TANK.PCX"),
          "a category keeps its key and file");
    CHECK(e[4].kind == TAGPU_ISE_UNKNOWN && e[5].kind == TAGPU_ISE_CATEGORY &&
          !strcmp(e[5].file, "sub\\plane.pcx") && e[6].kind == TAGPU_ISE_NUKE, "the rest, in file order");

    /* every key missing: TADR's defaults, and the fixed list */
    tagpu_iconcfg_parse("", 0, &c);
    CHECK(c.fill == 0 && c.transparent == 9 && c.selected == 89 && !c.custom && c.n == 0, "an empty file");
    {
        static const char* const t = "[Option]\nUseDefaultIcon=nope\nFillColor=999\n";
        tagpu_iconcfg_parse(t, strlen(t), &c);
    }
    CHECK(c.custom && c.fill == 0 && c.n == 0, "a custom set with no [Icon]: fill %d", c.fill);
    k = tagpu_iconset_plan(&c, sides, 0, e, TAGPU_ICONSET_PLAN);
    CHECK(k == 0, "a custom set with no lines and no sides plans nothing");
}

/* ---- 4. names ---- */
static void test_names(void)
{
    char longname[40];
    memset(longname, 'a', sizeof longname);
    CHECK(tagpu_iconset_ini_name_ok("TAESC.ini", 33) && tagpu_iconset_ini_name_ok("ta-zero_2.ini", 33),
          "ini names");
    CHECK(!tagpu_iconset_ini_name_ok("", 33) && !tagpu_iconset_ini_name_ok("..\\x.ini", 33) &&
          !tagpu_iconset_ini_name_ok("a b.ini", 33) && !tagpu_iconset_ini_name_ok("c:x", 33),
          "ini names refused");
    longname[32] = 0;
    CHECK(tagpu_iconset_ini_name_ok(longname, 33), "32 characters");
    longname[32] = 'a'; longname[33] = 0;
    CHECK(!tagpu_iconset_ini_name_ok(longname, 40), "33 characters");
    CHECK(!tagpu_iconset_ini_name_ok("abcdef", 3), "no NUL within the bytes it may read");
    CHECK(tagpu_iconset_relpath_ok(".\\Icon\\iconcfg.ini") && tagpu_iconset_relpath_ok("x.pcx") &&
          tagpu_iconset_relpath_ok("..\\up.pcx"), "relative paths");
    CHECK(!tagpu_iconset_relpath_ok("") && !tagpu_iconset_relpath_ok("\\root.ini") &&
          !tagpu_iconset_relpath_ok("/root.ini") && !tagpu_iconset_relpath_ok("C:\\x.ini") &&
          !tagpu_iconset_relpath_ok("x:y"), "absolute paths refused");
    CHECK(tagpu_iconset_side_ok("ARM", 30) && tagpu_iconset_side_ok("The Core", 30), "side names");
    CHECK(!tagpu_iconset_side_ok("", 30) && !tagpu_iconset_side_ok("a\\b", 30) &&
          !tagpu_iconset_side_ok("a/b", 30) && !tagpu_iconset_side_ok("a?", 30) &&
          !tagpu_iconset_side_ok("\x01", 30) && !tagpu_iconset_side_ok("ARMY", 4), "side names refused");
}

/* ---- 5. the atlas ---- */
static void test_atlas(void)
{
    static unsigned char logoPix[16][256], logoCls[16][256], setPix[64][484], setCls[64][484];
    static unsigned char atlas[256 * 256];
    static TAGPU_ICONSRC src[80];
    static TAGPU_ICONRECT rect[80];
    unsigned char pix[6] = { 9, 200, 250, 7, 9, 0 }, cls[6];
    int marks[3], i, j, k;

    tagpu_iconset_classify(pix, 6, 9, 200, 250, cls);
    CHECK(cls[0] == TAGPU_ITX_KEY && cls[1] == TAGPU_ITX_FILL && cls[2] == TAGPU_ITX_SEL &&
          cls[3] == TAGPU_ITX_DRAWN && cls[5] == TAGPU_ITX_DRAWN, "texel classes");
    tagpu_iconset_classify(pix, 6, 9, 9, 9, cls);
    CHECK(cls[0] == TAGPU_ITX_KEY && cls[4] == TAGPU_ITX_KEY, "Transparent wins over Fill and Selected");
    tagpu_iconset_classify(pix, 6, 1, 200, 200, cls);
    CHECK(cls[1] == TAGPU_ITX_FILL, "Fill wins over Selected");

    /* the worst case the art can be asked for: 16 logo frames of 16 px, drawn
       in indices 0..2 among others, and 64 set pictures of 22 px */
    for (i = 0; i < 16; i++) {
        for (k = 0; k < 256; k++) { logoPix[i][k] = (unsigned char)(k % 3 == 0 ? 0 : 10 + i); logoCls[i][k] = TAGPU_ITX_DRAWN; }
        logoCls[i][0] = TAGPU_ITX_KEY; logoPix[i][1] = 1; logoPix[i][2] = 2;
        src[i].pix = logoPix[i]; src[i].cls = logoCls[i]; src[i].w = 16; src[i].h = 16;
    }
    for (i = 0; i < 64; i++) {
        for (k = 0; k < 484; k++) setPix[i][k] = (unsigned char)(k % 5 == 0 ? 3 : 40 + (k % 7));
        setPix[i][1] = 4;                                    /* the set's transparent */
        setPix[i][2] = 6;                                    /* its fill */
        setPix[i][3] = 8;                                    /* its selected */
        tagpu_iconset_classify(setPix[i], 484, 4, 6, 8, setCls[i]);
        src[16 + i].pix = setPix[i]; src[16 + i].cls = setCls[i]; src[16 + i].w = 22; src[16 + i].h = 22;
    }
    CHECK(tagpu_iconart_pack(src, 80, atlas, 256, 256, rect, marks), "the worst case fits 256 x 256");
    CHECK(marks[0] == 4 && marks[1] == 5 && marks[2] == 6, "markers %d %d %d: the smallest no drawn texel holds",
          marks[0], marks[1], marks[2]);
    for (i = 0; i < 80; i++) {
        CHECK(rect[i].x >= 1 && rect[i].y >= 1 && rect[i].x + rect[i].w <= 255 && rect[i].y + rect[i].h <= 255,
              "rect %d inside the atlas with its gutter", i);
        for (j = 0; j < i; j++) {
            const int apart = rect[i].x >= rect[j].x + rect[j].w + 1 || rect[j].x >= rect[i].x + rect[i].w + 1 ||
                              rect[i].y >= rect[j].y + rect[j].h + 1 || rect[j].y >= rect[i].y + rect[i].h + 1;
            CHECK(apart, "rects %d and %d touch", i, j);
        }
    }
    {
        const TAGPU_ICONRECT* r = &rect[16];
        const unsigned char* a = atlas + (size_t)r->y * 256 + r->x;
        CHECK(a[0] == 3 && a[1] == marks[0] && a[2] == marks[1] && a[3] == marks[2] && a[4] == 40 + 4,
              "a set picture's texels: drawn, key, fill, sel, drawn");
        CHECK(atlas[(size_t)rect[0].y * 256 + rect[0].x] == marks[0] && atlas[0] == marks[0] &&
              atlas[256 * 256 - 1] == marks[0], "the logo's key and the uncovered atlas are the key");
    }

    /* one picture more than fits, a picture larger than the atlas, an empty one */
    CHECK(!tagpu_iconart_pack(src, 80, atlas, 128, 128, rect, marks), "80 pictures do not fit 128 x 128");
    src[0].w = 255;
    CHECK(!tagpu_iconart_pack(src, 1, atlas, 256, 256, rect, marks), "a picture wider than the atlas");
    src[0].w = 0;
    CHECK(!tagpu_iconart_pack(src, 1, atlas, 256, 256, rect, marks), "a picture with no pixels");
    src[0].w = 16;
    CHECK(tagpu_iconart_pack(src, 0, atlas, 256, 256, rect, marks) && marks[0] == 0 && marks[2] == 2,
          "no picture: markers 0, 1, 2");

    /* every index drawn: nothing can mark "not drawn" */
    for (k = 0; k < 256; k++) { setPix[0][k] = (unsigned char)k; setCls[0][k] = TAGPU_ITX_DRAWN; }
    src[0].pix = setPix[0]; src[0].cls = setCls[0]; src[0].w = 16; src[0].h = 16;
    CHECK(!tagpu_iconart_pack(src, 1, atlas, 256, 256, rect, marks), "every index drawn is refused");
    setCls[0][3] = TAGPU_ITX_FILL; setCls[0][5] = TAGPU_ITX_SEL; setCls[0][9] = TAGPU_ITX_KEY;
    CHECK(tagpu_iconart_pack(src, 1, atlas, 256, 256, rect, marks) && marks[0] == 3 && marks[1] == 5 &&
          marks[2] == 9, "the markers are chosen among the indices the classes free");
}

int main(void)
{
    test_pcx();
    test_ini();
    test_cfg();
    test_names();
    test_atlas();
    printf("iconsettest: %d check(s), %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
