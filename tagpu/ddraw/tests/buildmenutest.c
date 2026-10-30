/* buildmenutest.c -- the self-test of tagpu_buildmenu.c, for tests/buildmenutest.py.

   `buildmenutest` checks hand-written pages in retail's shape (no game file is in the repo)
   against the rule in tagpu_buildmenu.h, one case per step of it. It prints one line per
   failure and a summary; exit 0 when nothing failed.

   `buildmenutest dump` reads the protocol of .data/qbmenu/offline/crosscheck.py on stdin, one
   record a line, a name being the rest of the line:
       N <ndefs>                      the type count
       T <id> <armable 0|1> <name>    the type table; type_of() looks a name up in it without case
       B <tag>                        a builder case starts: no entries, nothing offered
       E <menu> <button> <name>       one of this builder's download entries, in file order
       G <orders 0|1> <count>         prints `Q <tag> <page> ...`, tagpu_buildmenu_pages
       P <page> <file>                applies this page's file
       X                              prints `O <tag> <id> ...`, the offered ids ascending
   for a cross-check against the Python model of the same rule. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "tagpu_buildmenu.h"

static unsigned s_checks, s_failed;

/* ---- the type table: case-insensitive, as 0x488B10's _stricmp ---------------------------- */

typedef struct { char name[64]; int armable; } Type;
static Type* s_types;
static unsigned s_ntypes;

static unsigned type_of(void* ctx, const char* name)
{
    unsigned i;
    (void)ctx;
    for (i = 1; i < s_ntypes; i++)
        if (s_types[i].name[0] && !strcasecmp(s_types[i].name, name)) return i;
    return 0;
}

static int armable(void* ctx, unsigned type)
{
    (void)ctx;
    return type < s_ntypes && s_types[type].armable;
}

static void types_reset(unsigned n)
{
    free(s_types);
    s_types = (Type*)calloc(n ? n : 1, sizeof *s_types);
    s_ntypes = s_types ? n : 0;
}

static void type_set(unsigned id, const char* name, int arm)
{
    if (id >= s_ntypes) return;
    snprintf(s_types[id].name, sizeof s_types[id].name, "%s", name);
    s_types[id].armable = arm;
}

/* ---- hand-written pages ------------------------------------------------------------------ */

enum { SOLAR = 1, MEX, WIN, CORBUILD, FLEA, HP, SIXTEEN, SEVENTEEN, NDEFS };

static void test_types(void)
{
    types_reset(NDEFS);
    type_set(SOLAR, "ARMSOLAR", 1);
    type_set(MEX, "ARMMEX", 1);
    type_set(WIN, "ARMWIN", 1);
    type_set(CORBUILD, "CORBUILD", 1);
    type_set(FLEA, "ARMFLEA", 0);                      /* mobile: def+0x22F != 0 */
    type_set(HP, "ARMHP", 1);
    type_set(SIXTEEN, "SIXTEENCHARNAMEX", 1);
    type_set(SEVENTEEN, "SEVENTEENCHARNAME", 1);
}

static char s_page[8192];

static void page_begin(void)
{
    snprintf(s_page, sizeof s_page,
             "[GADGET0]\r\n{\r\n\t[COMMON]\r\n\t{\r\n\tid=0;\r\n\tname=ARMPAN;\r\n\txpos=0;\r\n"
             "\typos=0;\r\n\twidth=128;\r\n\theight=352;\r\n\tactive=1;\r\n\tcommonattribs=0;\r\n\t}\r\n"
             "panel=ARMPAN;\r\n}\r\n");
}

static void gadget(int id, const char* name, int active, int common, int grey)
{
    size_t n = strlen(s_page);
    snprintf(s_page + n, sizeof s_page - n,
             "[GADGET]\r\n{\r\n\t[COMMON]\r\n\t{\r\n\tid=%d;\r\n\tassoc=0;\r\n\tname=%s;\r\n"
             "\txpos=0;\r\n\typos=0;\r\n\twidth=46;\r\n\theight=46;\r\n\tattribs=0;\r\n"
             "\tactive=%d;\r\n\tcommonattribs=%d;\r\n\t}\r\n\tgrayedout=%d;\r\n\tstages=0;\r\n}\r\n",
             id, name, active, common, grey);
}

static TAGPU_BM_ENTRY s_ent[16];
static unsigned s_nent;

static void entry(int menu, int button, const char* name)
{
    if (s_nent >= sizeof s_ent / sizeof s_ent[0]) return;
    s_ent[s_nent].menu = (unsigned char)menu;
    s_ent[s_nent].button = (unsigned char)button;
    snprintf(s_ent[s_nent].name, sizeof s_ent[s_nent].name, "%s", name);
    s_nent++;
}

static void fail(const char* what, const char* detail)
{
    s_failed++;
    printf("FAIL %s: %s\n", what, detail);
}

/* apply the page in s_page with s_ent and compare the offered set with `want` (0-terminated) */
static void expect_page(const char* what, unsigned page, const int* want, int want_ret)
{
    TAGPU_BM_ENGINE eng = { type_of, armable, NULL, NDEFS };
    unsigned char offered[NDEFS] = {0};
    char why[128] = "", got[256] = "", exp[256] = "";
    int ret, i;
    s_checks++;
    ret = tagpu_buildmenu_page((const unsigned char*)s_page, strlen(s_page), page, s_ent, s_nent,
                               &eng, offered, why, sizeof why);
    for (i = 1; i < NDEFS; i++)
        if (offered[i]) snprintf(got + strlen(got), sizeof got - strlen(got), " %s", s_types[i].name);
    for (i = 0; want[i]; i++)
        snprintf(exp + strlen(exp), sizeof exp - strlen(exp), " %s", s_types[want[i]].name);
    if (strcmp(got, exp) || ret != want_ret) {
        char d[700];
        snprintf(d, sizeof d, "offered{%s } want{%s }, returned %d want %d (%s)", got, exp, ret,
                 want_ret, why);
        fail(what, d);
    }
}

#define WANT(what, page, ret, ...) do { static const int w_[] = { __VA_ARGS__, 0 }; \
    expect_page(what, page, w_, ret); } while (0)
#define NONE(what, page) do { static const int w_[] = { 0 }; expect_page(what, page, w_, 0); } while (0)

static void pages_case(void)
{
    /* ARMCK1's shape: the panel, the type buttons and the pagers and tabs */
    s_nent = 0;
    page_begin();
    gadget(1, "ARMSOLAR", 1, 4, 0);
    gadget(1, "ARMMEX", 1, 4, 0);
    gadget(1, "ARMWIN", 1, 4, 0);
    gadget(1, "ARMPREV", 1, 0, 0);
    gadget(1, "ARMNEXT", 1, 0, 0);
    gadget(1, "BUILD", 1, 0, 0);
    gadget(1, "ORDERS", 1, 0, 0);
    gadget(5, "ARMLABEL", 1, 0, 0);
    WANT("ARMCK1's shape", 1, 3, SOLAR, MEX, WIN);

    /* a DL template: IGPATCH slots 4.., commonattribs 4, greyed; entries fill two of them */
    page_begin();
    gadget(1, "ARMPREV", 1, 0, 0);
    gadget(1, "ARMNEXT", 1, 0, 0);
    gadget(1, "BUILD", 1, 0, 0);
    gadget(1, "IGPATCH", 1, 4, 1);
    gadget(1, "IGPATCH", 1, 4, 1);
    gadget(1, "IGPATCH", 1, 4, 1);
    gadget(1, "IGPATCH", 1, 4, 1);
    s_nent = 0;
    entry(4, 0, "ARMHP");
    entry(4, 2, "ARMMEX");
    WANT("a DL template's filled slots", 3, 2, MEX, HP);

    /* MENU 0 never places; MENU - 1 != page does not; a slot past N does not */
    s_nent = 0;
    entry(0, 0, "ARMHP");
    entry(5, 1, "ARMMEX");
    entry(4, 4, "ARMSOLAR");                            /* slot 8, N is 7 */
    NONE("MENU 0, another page, a slot past N", 3);

    /* two entries on one slot: the later wins */
    s_nent = 0;
    entry(4, 1, "ARMHP");
    entry(4, 1, "ARMWIN");
    WANT("the later entry on one slot", 3, 1, WIN);

    /* a 17-character name: the greyed pass knows it whole, the press cut to 16 does not */
    s_nent = 0;
    entry(4, 0, "SEVENTEENCHARNAME");
    NONE("a 17-character entry name", 3);
    s_nent = 0;
    entry(4, 0, "SIXTEENCHARNAMEX");
    WANT("a 16-character entry name", 3, 1, SIXTEEN);

    /* a type that is not armable (def+0x22F != 0) */
    s_nent = 0;
    entry(4, 0, "ARMFLEA");
    NONE("a type with def+0x22F set", 3);

    /* an entry whose name has no NUL in its 32 bytes is not read */
    s_nent = 0;
    entry(4, 0, "ARMHP");
    memset(s_ent[0].name, 'A', sizeof s_ent[0].name);
    NONE("an entry name with no NUL", 3);

    /* the greyed pass covers records 0..N-1: gadget N keeps its file's grey bit */
    s_nent = 0;
    page_begin();
    gadget(1, "ARMSOLAR", 1, 4, 1);
    gadget(1, "ARMMEX", 1, 4, 1);
    WANT("the greyed pass's range", 1, 1, SOLAR);
    /* page 0 runs no greyed pass */
    NONE("page 0 keeps the file's grey bits", 0);
    page_begin();
    gadget(1, "ARMSOLAR", 1, 4, 0);
    gadget(1, "ARMWIN", 1, 0, 0);
    WANT("page 0's ungreyed buttons", 0, 2, SOLAR, WIN);
    /* the greyed pass greys a commonattribs-4 button with a name no type has */
    page_begin();
    gadget(1, "NOSUCHUNIT", 1, 4, 0);
    gadget(1, "ARMSOLAR", 1, 4, 0);
    gadget(1, "ARMWIN", 1, 4, 0);
    WANT("an unknown name", 1, 2, SOLAR, WIN);

    /* the tab test is case-sensitive: CORBUILD is a tab, corbuild a type */
    page_begin();
    gadget(1, "CORBUILD", 1, 4, 0);
    gadget(1, "ARMWIN", 1, 0, 0);
    WANT("CORBUILD is a tab", 1, 1, WIN);
    page_begin();
    gadget(1, "corbuild", 1, 4, 0);
    gadget(1, "ARMWIN", 1, 0, 0);
    WANT("corbuild is a type", 1, 2, WIN, CORBUILD);

    /* an inactive button and a non-button with a type's name are not pressed */
    page_begin();
    gadget(1, "ARMSOLAR", 0, 4, 0);
    gadget(0, "ARMMEX", 1, 4, 0);
    gadget(2, "ARMHP", 1, 4, 0);
    gadget(1, "ARMWIN", 1, 0, 0);
    WANT("inactive, id 0, id 2", 1, 1, WIN);

    /* a type offered by two pages is newly marked once */
    {
        TAGPU_BM_ENGINE eng = { type_of, armable, NULL, NDEFS };
        unsigned char offered[NDEFS] = {0};
        int a, b;
        s_checks++;
        a = tagpu_buildmenu_page((const unsigned char*)s_page, strlen(s_page), 1, NULL, 0, &eng,
                                 offered, NULL, 0);
        b = tagpu_buildmenu_page((const unsigned char*)s_page, strlen(s_page), 1, NULL, 0, &eng,
                                 offered, NULL, 0);
        if (a != 1 || b != 0) fail("newly marked", "a second page counted a type again");
    }
}

static void refusals(void)
{
    TAGPU_BM_ENGINE eng = { type_of, armable, NULL, NDEFS };
    unsigned char offered[NDEFS] = {0};
    char why[128];
    static const char missing[] = "[GADGET0]{[COMMON]{id=0}}";
    static const char none[] = "// nothing but a comment\r\n";
    s_checks += 3;
    why[0] = 0;
    if (tagpu_buildmenu_page((const unsigned char*)missing, sizeof missing - 1, 1, NULL, 0, &eng,
                             offered, why, sizeof why) != -1 || !why[0])
        fail("a missing ';'", "not refused, or no reason given");
    if (tagpu_buildmenu_page((const unsigned char*)"", 0, 1, NULL, 0, &eng, offered, why,
                             sizeof why) != -1)
        fail("0 bytes", "not refused");
    if (tagpu_buildmenu_page((const unsigned char*)none, sizeof none - 1, 1, NULL, 0, &eng,
                             offered, why, sizeof why) != -1)
        fail("no section", "not refused");
}

static void expect_pages(int orders, unsigned count, const char* want)
{
    unsigned char out[TAGPU_BM_PAGES];
    char got[64] = "", what[48];
    unsigned n = tagpu_buildmenu_pages(orders, count, out), i;
    s_checks++;
    for (i = 0; i < n; i++) snprintf(got + strlen(got), sizeof got - strlen(got), "%s%u", i ? "," : "", out[i]);
    snprintf(what, sizeof what, "pages(%d, %u)", orders, count);
    if (strcmp(got, want)) {
        char d[160];
        snprintf(d, sizeof d, "{%s} want {%s}", got, want);
        fail(what, d);
    }
}

static void expect_str(const char* what, int ok, const char* got, int want_ok, const char* want)
{
    s_checks++;
    if (ok != want_ok || (ok && strcmp(got, want))) {
        char d[400];
        snprintf(d, sizeof d, "%d \"%s\", want %d \"%s\"", ok, ok ? got : "", want_ok, want);
        fail(what, d);
    }
}

static void names(void)
{
    char out[300], longp[0x101], forty[41];
    int ok;
    expect_pages(0, 0, "");
    expect_pages(1, 0, "");
    expect_pages(1, 1, "0");
    expect_pages(0, 1, "");
    expect_pages(0, 4, "1,2,3");
    expect_pages(1, 5, "0,1,2,3,4");
    expect_pages(0, 12, "1,2,3,4,5,6,7");

    ok = tagpu_buildmenu_gui_path(out, sizeof out, "guis\\", "ARMCK1.GUI");
    expect_str("gui_path a page's own file", ok, out, 1, "guis\\ARMCK1.GUI");
    ok = tagpu_buildmenu_gui_path(out, sizeof out, "guis\\", "ARMDL");
    expect_str("gui_path the side's DL", ok, out, 1, "guis\\ARMDL.GUI");
    ok = tagpu_buildmenu_gui_path(out, sizeof out, "g.x\\", "ARMDL");
    expect_str("gui_path a dot before the backslash", ok, out, 1, "g.x\\ARMDL.GUI");
    ok = tagpu_buildmenu_gui_path(out, 16, "guis\\", "ARMCK1.GUI");
    expect_str("gui_path past its buffer", ok, out, 0, "");
    memset(longp, 'a', sizeof longp);
    longp[0x100] = 0;
    ok = tagpu_buildmenu_gui_path(out, sizeof out, longp, "ARMDL");
    expect_str("gui_path a prefix with no NUL in 0x100", ok, out, 0, "");

    memset(forty, 'N', 40);
    forty[40] = 0;
    ok = tagpu_buildmenu_page_name(out, sizeof out, forty, 3);
    expect_str("page_name cuts at 31", ok, out, 1, "NNNNNNNNNNNNNNNNNNNNNNNNNNNNNNN3.GUI");
    ok = tagpu_buildmenu_page_name(out, sizeof out, "ARMCK", 1);
    expect_str("page_name", ok, out, 1, "ARMCK1.GUI");
    ok = tagpu_buildmenu_page_name(out, 8, "ARMCK", 1);
    expect_str("page_name past its buffer", ok, out, 0, "");
}

/* ---- dump mode --------------------------------------------------------------------------- */

static unsigned char* read_file(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    unsigned char* b;
    long size;
    *n = 0;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    b = (unsigned char*)malloc((size_t)size + 1);
    if (b && fread(b, 1, (size_t)size, f) != (size_t)size) { free(b); b = NULL; }
    fclose(f);
    if (b) *n = (size_t)size;
    return b;
}

static int dump(void)
{
    static char line[4096];
    char tag[512] = "";
    TAGPU_BM_ENTRY* ent = NULL;
    unsigned nent = 0, cent = 0;
    unsigned char* offered = NULL;
    TAGPU_BM_ENGINE eng = { type_of, armable, NULL, 0 };
    while (fgets(line, sizeof line, stdin)) {
        size_t len = strcspn(line, "\r\n");
        line[len] = 0;
        if (line[0] == 'N' && line[1] == ' ') {
            types_reset((unsigned)strtoul(line + 2, NULL, 10));
            eng.ndefs = s_ntypes;
        } else if (line[0] == 'T' && line[1] == ' ') {
            char* p = line + 2;
            unsigned id = (unsigned)strtoul(p, &p, 10);
            int arm = (int)strtol(p, &p, 10);
            type_set(id, *p == ' ' ? p + 1 : p, arm);
        } else if (line[0] == 'B' && line[1] == ' ') {
            snprintf(tag, sizeof tag, "%.511s", line + 2);
            nent = 0;
            free(offered);
            offered = (unsigned char*)calloc(s_ntypes ? s_ntypes : 1, 1);
        } else if (line[0] == 'E' && line[1] == ' ') {
            char* p = line + 2;
            long menu = strtol(p, &p, 10), button = strtol(p, &p, 10);
            if (nent == cent) {
                TAGPU_BM_ENTRY* bigger;
                cent = cent ? cent * 2 : 16;
                if (!(bigger = (TAGPU_BM_ENTRY*)realloc(ent, cent * sizeof *ent))) return 2;
                ent = bigger;
            }
            memset(&ent[nent], 0, sizeof ent[nent]);
            ent[nent].menu = (unsigned char)menu;
            ent[nent].button = (unsigned char)button;
            snprintf(ent[nent].name, sizeof ent[nent].name, "%s", *p == ' ' ? p + 1 : p);
            nent++;
        } else if (line[0] == 'G' && line[1] == ' ') {
            char* p = line + 2;
            unsigned char pg[TAGPU_BM_PAGES];
            int orders = (int)strtol(p, &p, 10);
            unsigned count = (unsigned)strtoul(p, &p, 10), n, i;
            n = tagpu_buildmenu_pages(orders, count, pg);
            printf("Q %s", tag);
            for (i = 0; i < n; i++) printf(" %u", pg[i]);
            printf("\n");
        } else if (line[0] == 'P' && line[1] == ' ') {
            char* p = line + 2;
            unsigned page = (unsigned)strtoul(p, &p, 10);
            size_t n;
            unsigned char* blob = read_file(*p == ' ' ? p + 1 : p, &n);
            char why[256];
            if (blob && offered &&
                tagpu_buildmenu_page(blob, n, page, ent, nent, &eng, offered, why, sizeof why) < 0)
                fprintf(stderr, "%s page %u: %s\n", tag, page, why);
            free(blob);
        } else if (line[0] == 'X') {
            unsigned i;
            printf("O %s", tag);
            for (i = 1; offered && i < s_ntypes; i++) if (offered[i]) printf(" %u", i);
            printf("\n");
        }
    }
    free(ent);
    free(offered);
    free(s_types);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "dump")) return dump();
    test_types();
    pages_case();
    refusals();
    names();
    free(s_types);
    printf("buildmenutest: %u checks, %u failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
