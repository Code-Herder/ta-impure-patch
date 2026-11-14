/* classmap.c -- the shared core's feature grid and pathing maps for one map, from values on stdin; built and fed by
   tools/tasim/classcheck.py, which reads the map's files with the repository's readers and compares the answer with a
   running game's.

   It reads no file: every line on stdin is a value the caller read, and every rule is the core's -- each feature
   definition and move class through tasim_keys.c's one rule per key, the grid through tasim_map.c's LoadMap, the maps
   through tasim_class.c's fill. The wreck pool is the raised build's (TAGPU_LIM_WRECKS, tagpu_limits.h), the pool the
   game compared against allocates.

   stdin, one record a line:
     map W H SEA VERSION SURFACEMETAL LAVAWORLD
     attr HEX                      the TNT's attribute records, W * H of them
     fdef NAMEHEX                  a feature definition, in the engine's def order; then its section's keys,
     k KEYHEX VALUEHEX               one a line, each the bytes in hex (a TDF value can hold any byte but `;`),
     end                             and an end
     schema DEF X Z                a schema feature, DEF the resolved definition or -1
     class I                       moveinfo.tdf's CLASS<I>, then its keys and an end as for fdef
     strip CELL HMAX HMIN          after the load, the 2x2 extremes of a cell of the last column or row, which
                                   LoadMap never writes (tasim_map.h): the engine's values, to compare the rest
     go
   stdout:
     past_defs N
     grid HEX                      the W * H cells as the engine's 13 bytes
     class I FX FZ WM HEX          each present class: its map as little-endian dwords
   Exit 0, or 2 with the reason on stderr. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_limits.h"
#include "tasim_class.h"
#include "tasim_keys.h"
#include "tasim_map.h"

#define KEYS_MAX  256
#define DEFS_MAX  4096
#define LINE_MAX_ (64 * 1024 * 1024)

typedef struct {
    int  n;
    char key[KEYS_MAX][64];
    char val[KEYS_MAX][256];
} SECTION;

static int fail(const char* why)
{
    fprintf(stderr, "classmap: %s\n", why);
    return 2;
}

static int ieq(const char* a, const char* b)
{
    for (;; a++, b++) {
        unsigned ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

static const char* sec_get(void* ctx, const char* key)
{
    const SECTION* s = (const SECTION*)ctx;
    int i;
    for (i = s->n - 1; i >= 0; i--)             /* a key given twice keeps the last value (the engine's TDF reader) */
        if (ieq(s->key[i], key)) return s->val[i];
    return NULL;
}

/* the byte two hex digits at p spell, or -1. Not sscanf: glibc's sscanf measures its whole input string on every
   call, which over the attr line of a 1280 x 1280 map (13 MB of hex) is quadratic -- minutes, not milliseconds. */
static int hex_byte(const char* p)
{
    int v = 0, i;
    for (i = 0; i < 2; i++) {
        const char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

/* hex text into a C string of at most cap - 1 bytes; 0 when it is not hex or too long */
static int unhex(const char* h, char* out, size_t cap)
{
    size_t n = strlen(h), i;
    if (n % 2 || n / 2 >= cap) return 0;
    for (i = 0; i < n / 2; i++) {
        const int v = hex_byte(h + 2 * i);
        if (v <= 0) return 0;
        out[i] = (char)v;
    }
    out[n / 2] = 0;
    return 1;
}

static char* chomp(char* s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

/* the key lines up to `end` into s; 0 at a malformed line */
static int read_section(FILE* in, char* line, SECTION* s)
{
    s->n = 0;
    while (fgets(line, LINE_MAX_, in)) {
        char *k, *v;
        chomp(line);
        if (!strcmp(line, "end")) return 1;
        if (strncmp(line, "k ", 2) || s->n >= KEYS_MAX) return 0;
        k = line + 2;
        v = strchr(k, ' ');
        if (!v) return 0;
        *v++ = 0;
        if (!unhex(k, s->key[s->n], sizeof s->key[0]) || (*v && !unhex(v, s->val[s->n], sizeof s->val[0])))
            return 0;
        if (!*v) s->val[s->n][0] = 0;
        s->n++;
    }
    return 0;
}

static void put_hex(const unsigned char* p, size_t n)
{
    static const char hx[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        putchar(hx[p[i] >> 4]);
        putchar(hx[p[i] & 15]);
    }
}

int main(void)
{
    static TASIM_FEATUREDEF fd[DEFS_MAX];
    static TASIM_MOVECLASS cls[TS_CLASSES];
    static SECTION sec;
    char* line = (char*)malloc(LINE_MAX_);
    int32_t w = 0, h = 0, version = 0, surface = 0, lava = 0, nfd = 0, nsch = 0, sea = 0, i;
    uint8_t* attr = NULL;
    TS_MAPFEAT* sch = NULL;
    int32_t (*strip)[3] = NULL;         /* the last column and row: w + h - 1 cells */
    int32_t nstrip = 0;
    TS_MAP m;
    TS_CLASSMAPS cm;
    void *mm, *cmem;
    size_t n;
    int bad, go = 0;
    if (!line) return fail("no memory for a line");
    for (i = 0; i < TS_CLASSES; i++) tasim_moveclass_init(&cls[i]);
    while (!go && fgets(line, LINE_MAX_, stdin)) {
        chomp(line);
        if (!strncmp(line, "map ", 4)) {
            if (sscanf(line + 4, "%d %d %d %d %d %d", &w, &h, &sea, &version, &surface, &lava) != 6)
                return fail("a malformed map line");
            if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > TS_MAP_CELLS_MAX) return fail("map size");
            attr = (uint8_t*)calloc((size_t)w * h, 8);
            sch = (TS_MAPFEAT*)calloc(DEFS_MAX, sizeof *sch);
            strip = (int32_t(*)[3])calloc((size_t)w + h, sizeof *strip);
            if (!attr || !sch || !strip) return fail("no memory for the records");
        } else if (!strncmp(line, "attr ", 5)) {
            const char* p = line + 5;
            const size_t rec = version == 0x2000 ? 4 : 8, want = (size_t)w * h * rec;
            if (!attr || strlen(p) != 2 * want) return fail("the attr line's length");
            for (n = 0; n < want; n++) {
                const int v = hex_byte(p + 2 * n);
                if (v < 0) return fail("the attr line's hex");
                attr[n] = (uint8_t)v;
            }
        } else if (!strncmp(line, "fdef ", 5)) {
            char name[128];
            if (!unhex(line + 5, name, sizeof name)) return fail("a feature definition's name");
            if (nfd >= DEFS_MAX || !read_section(stdin, line, &sec)) return fail("a feature definition");
            tasim_featuredef_read(&fd[nfd++], name, sec_get, &sec);
        } else if (!strncmp(line, "schema ", 7)) {
            if (!sch || nsch >= DEFS_MAX ||
                sscanf(line + 7, "%d %d %d", &sch[nsch].def, &sch[nsch].x, &sch[nsch].z) != 3)
                return fail("a schema feature");
            nsch++;
        } else if (!strncmp(line, "strip ", 6)) {
            if (!strip || nstrip >= w + h ||
                sscanf(line + 6, "%d %d %d", &strip[nstrip][0], &strip[nstrip][1], &strip[nstrip][2]) != 3)
                return fail("a strip line");
            nstrip++;
        } else if (!strncmp(line, "class ", 6)) {
            int k;
            if (sscanf(line + 6, "%d", &k) != 1 || k < 0 || k >= TS_CLASSES) return fail("a class line");
            if (!read_section(stdin, line, &sec)) return fail("a class section");
            /* 0x42D328..0x42D3AA: the section's `name` copied, 0x440340 over the defaults 0x4402E0 */
            snprintf(cls[k].name, sizeof cls[k].name, "%s", sec_get(&sec, "name") ? sec_get(&sec, "name") : "");
            cls[k].present = 1;
            tasim_moveclass_read(&cls[k], sec_get, &sec);
        } else if (!strcmp(line, "go")) {
            go = 1;
        } else if (line[0]) {
            return fail("an unknown line");
        }
    }
    if (!go || !attr) return fail("no map, or no go");
    n = ts_map_bytes(w, h, TAGPU_LIM_WRECKS);
    mm = malloc(n ? n : 1);
    if (!mm || ts_map_init(&m, mm, n, w, h, (uint8_t)sea, TAGPU_LIM_WRECKS, fd, nfd) != TS_MAP_OK)
        return fail("the map is refused (tasim_map.h's bounds)");
    {
        TS_MAPLOAD in;
        memset(&in, 0, sizeof in);
        in.version = version;
        in.attr = attr;
        in.surface_metal = surface;
        in.lava = lava;
        in.schema = sch;
        in.nschema = nsch;
        if (ts_map_load(&m, &in)) return fail("LoadMap refuses the TNT's version");
    }
    for (i = 0; i < nstrip; i++) {
        const int32_t c = strip[i][0];
        if (c < 0 || c >= m.cells || (c % w != w - 1 && c / w != h - 1)) return fail("a strip cell off the strip");
        m.hmax[c] = (uint8_t)strip[i][1];
        m.hmin[c] = (uint8_t)strip[i][2];
    }
    n = ts_class_bytes(cls, w, h);
    cmem = malloc(n ? n : 1);
    if (!cmem || ts_class_init(&cm, cmem, n, cls, &m, NULL, &bad) != TS_CLASS_OK)
        return fail("a class is refused (tasim_class.h's bounds)");
    ts_class_fill_all(&cm);
    printf("past_defs %d\ngrid ", m.past_defs);
    for (i = 0; i < m.cells; i++) {
        uint8_t c[13];
        ts_map_cell(&m, i, c);
        put_hex(c, 13);
    }
    putchar('\n');
    for (i = 0; i < TS_CLASSES; i++) {
        const TS_CLASSMAP* c = &cm.c[i];
        size_t k;
        if (!c->present) continue;
        printf("class %d %d %d %u ", i, c->fx, c->fz, c->wm);
        for (k = 0; k < (size_t)cm.rows16 * (size_t)cm.w; k++) {
            const uint32_t v = c->map[k];
            const unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16),
                                         (unsigned char)(v >> 24) };
            put_hex(b, 4);
        }
        putchar('\n');
    }
    free(cmem);
    free(mm);
    free(attr);
    free(sch);
    free(strip);
    free(line);
    return 0;
}
