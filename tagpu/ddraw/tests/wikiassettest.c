/* wikiassettest.c -- the in-game wiki's 3D asset load (src/tagpu_wikiasset.c and the readers under
   it: tagpu_hpi.c, tagpu_vfs.c, tagpu_tdf.c) compiled natively, for tools/wiki/assetcheck.py and
   tools/wiki_regression.py's `asset` step. It includes the loader's source, so the fuzz reaches its
   static builders.

     wikiassettest load GAMEDIR PAK [--mounts FILE] [--exe EXE] [--language L]
         The load the DLL runs at the wiki's first open, over the install at GAMEDIR and the wiki's
         archive PAK (tools/hudpak.py --set wiki), printing the report the DLL's tagpu_wiki3d.json
         holds. The mount list is InitTAHPIAry's: the executable's main-archive pattern (read out of
         EXE, default GAMEDIR/TotalA.exe, at 0x5028CC with its argument at 0x5028D8), then *.CCX,
         *.UFO and *.HPI, each in name order without case, an archive taken when its last 36 bytes
         are "Copyright <any four bytes> Cavedog Entertainment" (OpenHAPIFile 0x4BDD70), a path
         taken once; or, with --mounts, FILE's archive names one a line, found in GAMEDIR without
         case (the game's own list, from its report). The folders' names are EXE's own (units
         0x503920, textures 0x503720, logos 0x504734, logos.GAF 0x503704, objects3d 0x5036E4,
         scripts 0x503740, anims 0x502E30, features 0x502C7C); the language "english" unless given.
         As the DLL's loader thread runs it, the sidebar is loaded first and handed to the 3D load,
         whose unit types then hold every layout side's cast types. The load's time and the
         arena's size go to stderr.
     wikiassettest side GAMEDIR PAK [same options] [--archive A] [--folders k=v,...]
         The sidebar's load, then the 3D load with it, printed as {"side": <the sidebar's report>,
         "minimap": {<entry>: {w, h, offx, offy, fnv} or null}, "units": [<the 3D load's unit
         types, in its order>]}.
     wikiassettest fuzz-hpi ARCHIVE N SEED
         N copies of ARCHIVE, each truncated, or with bytes flipped (half of them in its header and
         directory), or both, each opened from memory and every file of it read whole; a line per
         reason, and exit 1 when any refusal is not named. First, two archives made here: a chunked
         file whose size rounds past 32 bits, and one whose chunk table ends past 4 GiB, each read
         one byte, both of which must be refused by name.
     wikiassettest fuzz-3do ARCHIVE PATH N SEED
         N copies of the 3DO at PATH inside ARCHIVE, damaged the same ways, each built as the loader
         builds a model, its faces bound as misses, and walked by its report and by both piece
         orders the Create() mask joins.

   Built with -fsanitize=address,undefined by assetcheck.py, so a read or write out of bounds fails
   the run as well. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <dirent.h>
#include "../src/tagpu_wikiasset.c"

#define STAND_IN_BASE 0x10000000u       /* the arena's address in its 32-bit fields here */
#define ARENA_CAP     (32u << 20)

static unsigned char* slurp(const char* path, uint32_t* n)
{
    FILE* f = fopen(path, "rb");
    unsigned char* b;
    long m;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (m = ftell(f)) < 0 || m > 0x7FFFFFFFL || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    if (!(b = (unsigned char*)malloc((size_t)m + 1)) || fread(b, 1, (size_t)m, f) != (size_t)m) { fclose(f); free(b); return NULL; }
    fclose(f);
    *n = (uint32_t)m;
    return b;
}

/* ---- the executable's strings --------------------------------------------------------------------- */

static int exe_str(const unsigned char* b, uint32_t n, uint32_t va, char* out, size_t cap)
{
    uint32_t pe, nsec, opt, optsz, base, rva, i;
    if (n < 0x40 || b[0] != 'M' || b[1] != 'Z') return 0;
    pe = rd32(b + 0x3C);
    if (!inside(n, pe, 24) || memcmp(b + pe, "PE\0\0", 4)) return 0;
    nsec = rd16(b + pe + 6);
    optsz = rd16(b + pe + 20);
    opt = pe + 24;
    if (!inside(n, opt, optsz) || optsz < 32) return 0;
    base = rd32(b + opt + 28);
    rva = va - base;
    for (i = 0; i < nsec; i++) {
        const uint32_t s = opt + optsz + 40 * i;
        uint32_t vsize, vaddr, rsize, raddr, off;
        size_t k;
        if (!inside(n, s, 40)) return 0;
        vsize = rd32(b + s + 8); vaddr = rd32(b + s + 12); rsize = rd32(b + s + 16); raddr = rd32(b + s + 20);
        if (rva < vaddr || rva - vaddr >= (vsize > rsize ? vsize : rsize)) continue;
        off = raddr + rva - vaddr;
        for (k = 0; k + 1 < cap && inside(n, off, k + 1) && b[off + k]; k++) out[k] = (char)b[off + k];
        if (k + 1 >= cap || !inside(n, off, k + 1)) return 0;
        out[k] = 0;
        return 1;
    }
    return 0;
}

/* ---- the mount list ------------------------------------------------------------------------------- */

static int cmp_ci(const void* a, const void* b) { return strcasecmp(*(char* const*)a, *(char* const*)b); }

static int has_trailer(const char* path)
{
    static const char head[] = "Copyright ", tail[] = " Cavedog Entertainment";
    unsigned char t[36];
    FILE* f = fopen(path, "rb");
    int ok = 0;
    if (!f) return 0;
    if (!fseek(f, -36, SEEK_END) && fread(t, 1, 36, f) == 36)
        ok = !memcmp(t, head, 10) && !memcmp(t + 14, tail, 22);
    fclose(f);
    return ok;
}

static int mount(const char* dir, const char* first, WIKI3DIN* in)
{
    const char* pats[4];
    char* names[2048];
    int n = 0, p, i, k;
    DIR* d = opendir(dir);
    struct dirent* de;
    pats[0] = first; pats[1] = "*.CCX"; pats[2] = "*.UFO"; pats[3] = "*.HPI";
    if (!d) return 0;
    while (n < 2048 && (de = readdir(d)) != NULL)
        if ((names[n] = (char*)malloc(strlen(de->d_name) + 1)) != NULL) strcpy(names[n++], de->d_name);
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, cmp_ci);
    for (p = 0; p < 4; p++)
        for (i = 0; i < n; i++) {
            char full[2 * TAGPU_VFS_PATHMAX];
            if (!tagpu_vfs_match(names[i], pats[p])) continue;
            snprintf(full, sizeof full, "%s/%s", dir, names[i]);
            if (strlen(full) >= TAGPU_VFS_PATHMAX || !has_trailer(full)) continue;
            for (k = 0; k < in->narc && strcmp(in->arc[k], full); k++) {}
            if (k < in->narc || in->narc == TAGPU_VFS_ARCMAX) continue;
            strcpy(in->arc[in->narc++], full);
        }
    for (i = 0; i < n; i++) free(names[i]);
    return 1;
}

static int mount_from(const char* dir, const char* list, WIKI3DIN* in)
{
    FILE* f = fopen(list, "r");
    char line[512];
    if (!f) return 0;
    while (fgets(line, sizeof line, f) && in->narc < TAGPU_VFS_ARCMAX) {
        DIR* d;
        struct dirent* de;
        size_t k = strcspn(line, "\r\n");
        line[k] = 0;
        if (!k || !(d = opendir(dir))) continue;
        while ((de = readdir(d)) != NULL)
            if (!strcasecmp(de->d_name, line)) {
                snprintf(in->arc[in->narc], TAGPU_VFS_PATHMAX, "%s/%s", dir, de->d_name);
                break;
            }
        if (!de) snprintf(in->arc[in->narc], TAGPU_VFS_PATHMAX, "%s/%s", dir, line);   /* refused, named */
        in->narc++;
        closedir(d);
    }
    fclose(f);
    return 1;
}

/* The load's input as the DLL's game thread gathers it, from the install at GAMEDIR and the
   executable's strings: the folder names (each overridable by --folders, as a mod's loader DLL
   rewrites them in memory), the mount list (its first pattern overridable by --archive, for the
   same reason) and the wiki's data. 0 with a message on stderr. */
typedef struct { WIKI3DIN in; unsigned char* pak; WIKIDATA* data; } INPUT;

static int input_make(int argc, char** argv, INPUT* I)
{
    const char *dir = argv[2], *pak = argv[3], *mounts = NULL, *exe = NULL, *lang = "english", *archive = NULL, *folders = NULL;
    char exepath[1024], first[64], arg[16], err[256];
    WIKI3DIN* in = &I->in;
    unsigned char* eb;
    uint32_t en, pn;
    int i;
    struct { uint32_t va; const char* key; char* dst; } names[] = {
        { 0x503920u, "units", in->names.units }, { 0x503720u, "textures", in->names.textures },
        { 0x504734u, "logos", in->names.logos }, { 0x503704u, "logos_gaf", in->names.logos_gaf },
        { 0x5036E4u, "objects3d", in->names.objects3d }, { 0x503740u, "scripts", in->names.scripts },
        { 0x502E30u, "anims", in->names.anims }, { 0x502C7Cu, "features", in->names.features },
        { 0x502820u, "gui", in->names.gui }, { 0x503360u, "gamedata", in->names.gamedata },
        { 0x505FE4u, "unitpics", in->names.unitpics }, { 0x503730u, "download", in->names.download },
        { 0x50925Cu, "commongui", in->names.commongui }, { 0x5031BCu, "palettes", in->names.palettes },
        { 0x5033A4u, "palette", in->names.palette }, { 0x5031C8u, "guipal", in->names.guipal } };
    memset(I, 0, sizeof *I);
    for (i = 4; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--mounts")) mounts = argv[i + 1];
        else if (!strcmp(argv[i], "--exe")) exe = argv[i + 1];
        else if (!strcmp(argv[i], "--language")) lang = argv[i + 1];
        else if (!strcmp(argv[i], "--archive")) archive = argv[i + 1];
        else if (!strcmp(argv[i], "--folders")) folders = argv[i + 1];
        else { fprintf(stderr, "wikiassettest: unknown option %s\n", argv[i]); return 0; }
    }
    if (!exe) { snprintf(exepath, sizeof exepath, "%s/TotalA.exe", dir); exe = exepath; }
    if (!(eb = slurp(exe, &en))) { fprintf(stderr, "wikiassettest: cannot read %s\n", exe); return 0; }
    for (i = 0; i < (int)(sizeof names / sizeof *names); i++)
        if (!exe_str(eb, en, names[i].va, names[i].dst, WIKI3D_NAMEMAX)) { fprintf(stderr, "wikiassettest: no folder name at 0x%X\n", names[i].va); return 0; }
    if (!exe_str(eb, en, 0x5028CCu, first, sizeof first) || !exe_str(eb, en, 0x5028D8u, arg, sizeof arg)) {
        fprintf(stderr, "wikiassettest: no archive pattern in %s\n", exe);
        return 0;
    }
    free(eb);
    if (strstr(first, "%s")) {
        char t[64];
        snprintf(t, sizeof t, first, arg);
        strcpy(first, t);
    }
    if (archive) snprintf(first, sizeof first, "%s", archive);
    /* --folders gui=guiM,unitpics=unitpicM,... */
    while (folders && *folders) {
        const char* eq = strchr(folders, '=');
        const char* end = strchr(folders, ',');
        size_t kn, vn;
        if (!end) end = folders + strlen(folders);
        if (!eq || eq > end) { fprintf(stderr, "wikiassettest: --folders takes name=folder pairs\n"); return 0; }
        kn = (size_t)(eq - folders);
        vn = (size_t)(end - eq - 1);
        for (i = 0; i < (int)(sizeof names / sizeof *names); i++)
            if (strlen(names[i].key) == kn && !strncmp(names[i].key, folders, kn)) break;
        if (i == (int)(sizeof names / sizeof *names) || vn >= WIKI3D_NAMEMAX) { fprintf(stderr, "wikiassettest: --folders: %.*s\n", (int)kn, folders); return 0; }
        memcpy(names[i].dst, eq + 1, vn);
        names[i].dst[vn] = 0;
        folders = *end ? end + 1 : end;
    }
    snprintf(in->names.language, sizeof in->names.language, "%s", lang);
    snprintf(in->cwd, sizeof in->cwd, "%s", dir);
    if (!(mounts ? mount_from(dir, mounts, in) : mount(dir, first, in))) { fprintf(stderr, "wikiassettest: no mount list\n"); return 0; }
    if (!(I->pak = slurp(pak, &pn))) { fprintf(stderr, "wikiassettest: cannot read %s\n", pak); return 0; }
    if (!(I->data = tagpu_wikidata_load(I->pak, pn, err, sizeof err))) { fprintf(stderr, "wikiassettest: %s\n", err); return 0; }
    in->data = I->data;
    in->pak = I->pak;
    in->npak = pn;
    return 1;
}

static void input_free(INPUT* I)
{
    tagpu_wikidata_free(I->data);
    free(I->pak);
}

static double ms_since(const struct timespec* t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0->tv_nsec) / 1e6;
}

static int cmd_load(int argc, char** argv)
{
    static INPUT I;
    unsigned char* arena;
    const WIKI3D* w;
    const WIKISIDE* side;
    TAGPU_VFS* vfs;
    char* report = NULL;
    struct timespec t0;
    if (!input_make(argc, argv, &I)) return 2;
    if (!(arena = (unsigned char*)calloc(1, ARENA_CAP))) return 2;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    vfs = tagpu_vfs_open(I.in.cwd, (const char (*)[TAGPU_VFS_PATHMAX])I.in.arc, I.in.narc, I.in.names.language);
    side = tagpu_wikiside_load(&I.in, vfs, &report);
    w = tagpu_wiki3d_load(&I.in, vfs, side, arena, STAND_IN_BASE, ARENA_CAP, NULL);
    if (!w) { fprintf(stderr, "wikiassettest: the load published nothing\n"); return 1; }
    fputs(w->report, stdout);
    fprintf(stderr, "wikiassettest: loaded in %.1f ms - arena %lu bytes; %d mounts; the sidebar %s\n", ms_since(&t0),
            (unsigned long)w->bytes, I.in.narc, side ? "loaded" : "refused");
    tagpu_vfs_close(vfs);
    free((void*)side);
    free(report);
    free(arena);
    input_free(&I);
    return 0;
}

/* The sidebar's load as the DLL's loader thread runs it -- the sidebar, then the 3D load with it --
   printed as {"side": <its report>, "minimap": {<entry>: {w, h, offx, offy, fnv} or null},
   "units": [<name>...]}: the report tagpu_wikiside.json holds, each stage's minimap picture and
   the 3D load's unit types. The frames' bound the block promises is checked again here. */
static int cmd_side(int argc, char** argv)
{
    static INPUT I;
    unsigned char* arena;
    const WIKISIDE* side;
    const WIKI3D* w;
    TAGPU_VFS* vfs;
    char* report = NULL;
    struct timespec t0;
    double side_ms;
    int i;
    if (!input_make(argc, argv, &I)) return 2;
    if (!(arena = (unsigned char*)calloc(1, ARENA_CAP))) return 2;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    vfs = tagpu_vfs_open(I.in.cwd, (const char (*)[TAGPU_VFS_PATHMAX])I.in.arc, I.in.narc, I.in.names.language);
    side = tagpu_wikiside_load(&I.in, vfs, &report);
    side_ms = ms_since(&t0);
    if (!report) { fprintf(stderr, "wikiassettest: the sidebar's load wrote no report\n"); return 1; }
    if (side && (strcmp(side->report, report) || !side_frames_inside(side))) {
        fprintf(stderr, "wikiassettest: the block's report or frames are not what the load made\n");
        return 1;
    }
    w = tagpu_wiki3d_load(&I.in, vfs, side, arena, STAND_IN_BASE, ARENA_CAP, NULL);
    printf("{\"side\":%s,\"minimap\":{", report);
    for (i = 0; w && i < w->nstage; i++) {
        const WIKI3DSTAGE* st = &w->stage[i];
        printf("%s\"%s\":", i ? "," : "", st->entry);
        if (st->minimap)
            printf("{\"w\":%d,\"h\":%d,\"offx\":%d,\"offy\":%d,\"fnv\":\"%08lx\"}", st->mm_w, st->mm_h, st->mm_offx,
                   st->mm_offy, (unsigned long)fnv(FNV0, st->minimap, (size_t)(st->mm_w * st->mm_h)));
        else printf("null");
    }
    printf("},\"units\":[");
    for (i = 0; w && i < w->nunit; i++) printf("%s\"%s\"", i ? "," : "", w->unit[i].name);
    printf("]}\n");
    fprintf(stderr, "wikiassettest: the sidebar %s in %.1f ms (%d screens, a %d-row sheet); %d mounts\n",
            side ? "loaded" : "REFUSED", side_ms, side ? side->layout.nscreen : 0, side ? side->sheet_h : 0, I.in.narc);
    tagpu_vfs_close(vfs);
    free((void*)side);
    free(report);
    free(arena);
    input_free(&I);
    return 0;
}

/* ---- the fuzz ------------------------------------------------------------------------------------- */

static uint32_t s_rng;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

/* a damaged copy of `b`: truncated, flipped (half the flips within the first `hot` bytes), or both */
static unsigned char* damage(const unsigned char* b, uint32_t n, uint32_t hot, uint32_t* out)
{
    unsigned char* c = (unsigned char*)malloc(n ? n : 1);
    const uint32_t mode = rnd() % 3;
    uint32_t m = n, k, flips;
    memcpy(c, b, n);
    if (mode != 1 && n) m = rnd() % n;
    if (mode != 0 && m) {
        flips = 1 + rnd() % 8;
        for (k = 0; k < flips; k++) {
            const uint32_t span = (rnd() & 1) && hot ? (hot < m ? hot : m) : m;
            c[rnd() % span] ^= (unsigned char)(1 + rnd() % 255);
        }
    }
    *out = m;
    return c;
}

typedef struct { const unsigned char* b; } MEM;
static int mem_read(void* ctx, uint32_t off, void* dst, uint32_t n)
{
    memcpy(dst, ((MEM*)ctx)->b + off, n);      /* tagpu_hpi checks the range against size first */
    return 1;
}

#define REASONS 64
static char s_reason[REASONS][96];
static int  s_count[REASONS], s_nreason, s_unnamed;

static void tally(const char* why)
{
    char key[96];
    int i;
    size_t k;
    if (!why || !*why) { s_unnamed++; return; }
    /* the reason without its numbers, decimal or hex, so one kind of refusal is one line */
    {
        size_t r = 0;
        k = 0;
        while (why[r] && k + 1 < sizeof key) {
            if (why[r] >= '0' && why[r] <= '9') {
                while (why[r] && strchr("0123456789abcdefABCDEFx", why[r])) r++;
                key[k++] = '#';
            } else key[k++] = why[r++];
        }
        key[k] = 0;
    }
    for (i = 0; i < s_nreason && strcmp(s_reason[i], key); i++) {}
    if (i == s_nreason) {
        if (s_nreason == REASONS) i = REASONS - 1;
        else strcpy(s_reason[s_nreason++], key);
    }
    s_count[i]++;
}

static void report_tally(const char* what, int n, int clean)
{
    int i;
    printf("%s: %d copies: %d taken whole, %d with a refusal; %d refusals unnamed; the refusals by kind:\n",
           what, n, clean, n - clean, s_unnamed);
    for (i = 0; i < s_nreason; i++) printf("  %5d  %s\n", s_count[i], s_reason[i]);
}

/* An unkeyed archive of one chunked file, "big", whose record says `size` bytes at `data`, with
   `pad` bytes after the directory; a read of its first byte must be refused by name. */
static int hpi_one(uint32_t size, uint32_t data, uint32_t pad)
{
    const uint32_t dir = 20, end = dir + 8 + 9 + 4 + 9;
    unsigned char* b = (unsigned char*)calloc(1, end + pad);
    unsigned char out[1];
    char why[200] = "";
    MEM mem;
    TAGPU_HPISRC src;
    TAGPU_HPI* a;
    int e, ok;
#define W32(p, v) do { (p)[0] = (unsigned char)(v); (p)[1] = (unsigned char)((v) >> 8); \
                       (p)[2] = (unsigned char)((v) >> 16); (p)[3] = (unsigned char)((v) >> 24); } while (0)
    if (!b) return 0;
    W32(b, 0x49504148u);                    /* "HAPI" */
    W32(b + 4, 0x00010000u);
    W32(b + 8, end);                        /* the directory's end, its key 0, its start */
    W32(b + 16, dir);
    W32(b + dir, 1u);                       /* the root: one entry, listed after it */
    W32(b + dir + 4, dir + 8);
    W32(b + dir + 8, dir + 17);             /* the entry: its name, its record, a file */
    W32(b + dir + 12, dir + 21);
    memcpy(b + dir + 17, "big", 4);
    W32(b + dir + 21, data);                /* the record: data, size, chunked */
    W32(b + dir + 25, size);
    b[dir + 29] = 1;
#undef W32
    mem.b = b;
    src.ctx = &mem;
    src.size = end + pad;
    src.read = mem_read;
    if (!(a = tagpu_hpi_open(&src, why, sizeof why)) || (e = tagpu_hpi_find(a, "big")) < 0) {
        fprintf(stderr, "wikiassettest: the made archive did not open: %s\n", why);
        if (a) tagpu_hpi_close(a);
        free(b);
        return 0;
    }
    ok = !tagpu_hpi_read(a, e, 0, 1, out, why, sizeof why) && why[0];
    printf("fuzz-hpi: a chunked file of %lu bytes at 0x%lx: %s\n", (unsigned long)size, (unsigned long)data,
           ok ? why : "NOT refused by name");
    tagpu_hpi_close(a);
    free(b);
    return ok;
}

static int cmd_fuzz_hpi(int argc, char** argv)
{
    uint32_t n, m, hot;
    unsigned char* b;
    int cases = atoi(argv[3]), i, clean = 0;
    s_rng = (uint32_t)strtoul(argv[4], NULL, 10) | 1u;
    (void)argc;
    if (!hpi_one(0xFFFFFFF0u, 64, 64) || !hpi_one(1, 0xFFFFFFFEu, 64)) return 1;
    if (!(b = slurp(argv[2], &n))) { fprintf(stderr, "wikiassettest: cannot read %s\n", argv[2]); return 2; }
    hot = n >= 12 ? rd32(b + 8) + 64 : n;           /* the header and the directory */
    for (i = 0; i < cases; i++) {
        unsigned char* c = damage(b, n, hot, &m);
        MEM mem;
        TAGPU_HPISRC src;
        TAGPU_HPI* a;
        char why[200] = "";
        int e, ok = 1;
        mem.b = c;
        src.ctx = &mem;
        src.size = m;
        src.read = mem_read;
        if (!(a = tagpu_hpi_open(&src, why, sizeof why))) { tally(why); free(c); continue; }
        for (e = 0; e < tagpu_hpi_count(a); e++) {
            const uint32_t sz = tagpu_hpi_size(a, e);
            unsigned char* out;
            if (tagpu_hpi_is_dir(a, e)) continue;
            if (sz > TAGPU_HPI_READMAX) { tally("a file past one read's bound"); ok = 0; continue; }
            if (!(out = (unsigned char*)malloc(sz ? sz : 1))) continue;
            why[0] = 0;
            if (!tagpu_hpi_read(a, e, 0, sz, out, why, sizeof why)) { tally(why); ok = 0; }
            free(out);
        }
        tagpu_hpi_close(a);
        clean += ok;
        free(c);
    }
    report_tally("fuzz-hpi", cases, clean);
    free(b);
    return s_unnamed ? 1 : 0;
}

static int cmd_fuzz_3do(int argc, char** argv)
{
    uint32_t n, m, an;
    unsigned char *ab, *b;
    int cases = atoi(argv[4]), i, clean = 0;
    MEM mem;
    TAGPU_HPISRC src;
    TAGPU_HPI* arc;
    char why[200] = "";
    int e;
    unsigned char* arena;
    (void)argc;
    s_rng = (uint32_t)strtoul(argv[5], NULL, 10) | 1u;
    if (!(ab = slurp(argv[2], &an))) { fprintf(stderr, "wikiassettest: cannot read %s\n", argv[2]); return 2; }
    mem.b = ab;
    src.ctx = &mem;
    src.size = an;
    src.read = mem_read;
    if (!(arc = tagpu_hpi_open(&src, why, sizeof why)) || (e = tagpu_hpi_find(arc, argv[3])) < 0 || tagpu_hpi_is_dir(arc, e)) {
        fprintf(stderr, "wikiassettest: no %s in %s (%s)\n", argv[3], argv[2], why);
        return 2;
    }
    n = tagpu_hpi_size(arc, e);
    if (!(b = (unsigned char*)malloc(n)) || !tagpu_hpi_read(arc, e, 0, n, b, why, sizeof why)) { fprintf(stderr, "wikiassettest: %s\n", why); return 2; }
    tagpu_hpi_close(arc);
    if (!(arena = (unsigned char*)calloc(1, ARENA_CAP))) return 2;
    for (i = 0; i < cases; i++) {
        unsigned char* c = damage(b, n, n < 4096 ? n : 4096, &m);
        LOADER L;
        MODEL md;
        SB sb;
        int t;
        memset(&L, 0, sizeof L);
        memset(&md, 0, sizeof md);
        memset(&sb, 0, sizeof sb);
        L.a.base = arena;
        L.a.base32 = STAND_IN_BASE;
        L.a.cap = L.a.committed = ARENA_CAP;
        L.a.used = 8;
        why[0] = 0;
        if (!(md.root = build_model(&L, c, m, &md.pieces, &md.faces, md.why, sizeof md.why))) tally(md.why);
        else {
            md.ok = 1;
            for (t = 0; t < L.ntex; t++) L.tex[t].kind = T_MISS;
            bind_all(&L);
            model_report(&sb, &L, &md);
            {
                uint32_t bits[MAXPIECE / 32];
                char hw[64];
                unit_hides(&L.a, NULL, 0, md.root, TAGPU_COB_HIDE, bits, hw, sizeof hw);   /* both piece walks */
            }
            clean++;
        }
        free(sb.s);
        free(L.tex);
        free(L.bind);
        free(c);
    }
    report_tally("fuzz-3do", cases, clean);
    free(arena);
    free(b);
    free(ab);
    return s_unnamed ? 1 : 0;
}

/* ---- the sidebar's fuzz, over synthetic files only ------------------------------------------------
   An install the test writes itself, every file of it made here from our own numbers (no byte of
   the game): sidedata, ten FBIs whose casting the rule binds whole, a download TDF, eight .GUI
   screens (one a 0-byte page stop), five GAFs whose frames are plain, run-length coded and stacked
   from sub-frames, a PCX and the palette files. Then N rounds, each damaging one file (or none) --
   truncated, bytes flipped, the grammar's own characters scattered in, a structural count or offset
   of a GAF set huge, emptied, removed -- and running the whole sidebar load over it; and N rounds of
   the positional reader over damaged and pathological texts, of the PCX reader and of the number
   reader. Every load must end in a block whose layout passes the check, whose frames lie in its
   sheet and whose every casting the pack's reader accepts and reads back as the block holds it
   (casting_agrees), or in a named refusal; the sanitizers do the rest. */

typedef struct { unsigned char* b; uint32_t n, cap; } BUF;

static void bput(BUF* o, const void* p, uint32_t n)
{
    if (!n) return;
    if (o->n + n > o->cap) {
        o->cap = (o->n + n) * 2 + 256;
        if (!(o->b = (unsigned char*)realloc(o->b, o->cap))) { fprintf(stderr, "wikiassettest: out of memory\n"); exit(2); }
    }
    memcpy(o->b + o->n, p, n);
    o->n += n;
}
static void b8(BUF* o, unsigned v) { unsigned char c = (unsigned char)v; bput(o, &c, 1); }
static void b16(BUF* o, unsigned v) { b8(o, v & 0xFF); b8(o, (v >> 8) & 0xFF); }
static void b32(BUF* o, uint32_t v) { b16(o, v & 0xFFFF); b16(o, v >> 16); }
static void p32(BUF* o, uint32_t at, uint32_t v) { wr32(o->b + at, v); }

/* the structural offsets of the GAFs built, where the "huge counts" damage writes */
static uint32_t s_hot[64];
static int s_nhot;
static void hot(uint32_t at) { if (s_nhot < 64) s_hot[s_nhot++] = at; }

enum { FR_PLAIN, FR_RLE, FR_STACK };
typedef struct { const char* name; int nframe, w, h, hx, hy, kind; } SEQSPEC;

/* a frame's pixels: a pattern of our own, the key (0) on its border */
static unsigned char px_at(int x, int y, int w, int h, int salt)
{
    return (unsigned char)(x == 0 || y == 0 || x == w - 1 || y == h - 1 ? 0 : 16 + (x * 7 + y * 3 + salt) % 200);
}

static uint32_t put_leaf(BUF* o, int w, int h, int hx, int hy, int rle, int salt)
{
    const uint32_t fo = o->n;
    int x, y;
    hot(fo);
    b16(o, (unsigned)w); b16(o, (unsigned)h); b16(o, (unsigned)hx & 0xFFFF); b16(o, (unsigned)hy & 0xFFFF);
    b8(o, 0); b8(o, (unsigned)rle); b8(o, 0); b8(o, 0); b32(o, 0); b32(o, 0); b32(o, 0);
    p32(o, fo + 16, o->n);
    if (!rle) {
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) b8(o, px_at(x, y, w, h, salt));
        return fo;
    }
    /* each row: a transparent run of 1, a repeat of 3, then literals of the rest */
    for (y = 0; y < h; y++) {
        BUF r = { 0 };
        int rest = w - 4;
        b8(&r, (1 << 1) | 1);
        if (w >= 4) { b8(&r, ((3 - 1) << 2) | 2); b8(&r, px_at(1, y, w, h, salt)); }
        x = 4;
        while (rest > 0) {
            const int n = rest > 64 ? 64 : rest;
            int k;
            b8(&r, (unsigned)((n - 1) << 2));
            for (k = 0; k < n; k++) b8(&r, px_at(x + k, y, w, h, salt));
            x += n;
            rest -= n;
        }
        b16(o, r.n);
        bput(o, r.b, r.n);
        free(r.b);
    }
    return fo;
}

static uint32_t put_frame(BUF* o, const SEQSPEC* q, int k)
{
    uint32_t fo, tab, a, b;
    if (q->kind != FR_STACK) return put_leaf(o, q->w, q->h, q->hx, q->hy, q->kind == FR_RLE, k);
    /* a stack: a body and its lettering, each at its own hotspot */
    fo = o->n;
    hot(fo);
    b16(o, (unsigned)q->w); b16(o, (unsigned)q->h); b16(o, (unsigned)q->hx); b16(o, (unsigned)q->hy);
    b8(o, 0); b8(o, 0); b8(o, 2); b8(o, 0); b32(o, 0); b32(o, 0); b32(o, 0);
    tab = o->n;
    p32(o, fo + 16, tab);
    hot(tab);
    b32(o, 0); b32(o, 0);
    a = put_leaf(o, q->w, q->h, q->hx, q->hy, 0, k);
    b = put_leaf(o, q->w / 2, q->h / 2, q->hx - 2, q->hy - 1, 1, k + 7);
    p32(o, tab, a);
    p32(o, tab + 4, b);
    return fo;
}

static BUF make_gaf(const SEQSPEC* q, int n)
{
    BUF o = { 0 };
    int i, k;
    b32(&o, 0x00010100u); b32(&o, (uint32_t)n); b32(&o, 0);
    hot(4);
    for (i = 0; i < n; i++) b32(&o, 0);
    for (i = 0; i < n; i++) {
        const uint32_t so = o.n;
        char name[32];
        uint32_t tab;
        p32(&o, 12 + 4 * i, so);
        hot(so);
        b16(&o, (unsigned)q[i].nframe); b16(&o, 1); b32(&o, 0);
        memset(name, 0, sizeof name);
        snprintf(name, sizeof name, "%s", q[i].name);
        bput(&o, name, 32);
        tab = o.n;
        for (k = 0; k < q[i].nframe; k++) { b32(&o, 0); b32(&o, 0); }
        for (k = 0; k < q[i].nframe; k++) p32(&o, tab + 8 * k, put_frame(&o, &q[i], k));
    }
    return o;
}

static BUF make_text(const char* t)
{
    BUF o = { 0 };
    bput(&o, t, (uint32_t)strlen(t));
    return o;
}

static BUF make_pcx(int w, int h)
{
    BUF o = { 0 };
    unsigned char hd[128];
    int x, y;
    memset(hd, 0, sizeof hd);
    hd[0] = 10; hd[1] = 5; hd[2] = 1; hd[3] = 8;
    wr16(hd + 8, (uint16_t)(w - 1)); wr16(hd + 10, (uint16_t)(h - 1));
    hd[65] = 1;
    wr16(hd + 66, (uint16_t)(w + (w & 1)));
    bput(&o, hd, 128);
    for (y = 0; y < h; y++)
        for (x = 0; x < w + (w & 1); x++) {
            const unsigned char v = px_at(x, y, w, h, 3);
            if (v >= 0xC0) b8(&o, 0xC1);
            b8(&o, v);
        }
    return o;
}

#define GUI_HEAD(name, panel, h) "[GADGET0]\r\n{\r\n\t[COMMON]\r\n\t{\r\n\t\tid=0;\r\n\t\tname=" name ";\r\n\t\txpos=0;\r\n\t\typos=128;\r\n" \
    "\t\twidth=128;\r\n\t\theight=" h ";\r\n\t\tattribs=0;\r\n\t\tactive=1;\r\n\t}\r\n\tpanel=" panel ";\r\n}\r\n"
#define GUI_BTN(sec, name, x, y, w, h, attribs, common) "[" sec "]{[COMMON]{id=1;name=" name ";xpos=" x ";ypos=" y ";width=" w \
    ";height=" h ";attribs=" attribs ";commonattribs=" common ";active=1;}status=0;text=;stages=0;}\r\n"

typedef struct { const char* path; BUF body; } SYNFILE;
static SYNFILE s_files[48];
static int s_nfiles;

static void syn(const char* path, BUF body)
{
    s_files[s_nfiles].path = path;
    s_files[s_nfiles++].body = body;
}

static void synth_install(void)
{
    static const SEQSPEC common[] = {
        { "ARMORDERS", 3, 58, 18, 0, 0, FR_STACK }, { "ARMBUILD", 3, 58, 18, 0, 0, FR_RLE },
        { "ARMPREV", 3, 44, 16, 0, 0, FR_PLAIN }, { "ARMNEXT", 3, 44, 16, 0, 0, FR_PLAIN },
        { "ARMSTOP", 3, 54, 30, 0, 0, FR_RLE }, { "BUTTONS0", 8, 96, 20, 0, 0, FR_RLE },
        { "BackTile", 9, 16, 16, 5, 7, FR_PLAIN }, { "ARMPAN", 1, 128, 96, 0, 0, FR_RLE },
        { "IGPATCH", 1, 32, 32, 0, 0, FR_PLAIN } };
    static const SEQSPEC page[] = { { "ARMMEX", 3, 32, 32, 0, 0, FR_RLE } };
    static const SEQSPEC intgaf[] = { { "PANELSIDE", 1, 128, 200, 2, 3, FR_RLE } };
    static const SEQSPEC fx[] = { { "radlogo", 10, 4, 4, 1, 1, FR_PLAIN }, { "radlogohigh", 1, 6, 6, 2, 2, FR_STACK } };
    static const SEQSPEC gadget[] = { { "ARMSOLAR", 3, 32, 32, 281, 171, FR_PLAIN } };
    BUF pal = { 0 }, shd = { 0 }, alp = { 0 }, gpal = { 0 };
    int i;
    s_nhot = 0;
    for (i = 0; i < 256; i++) { b8(&pal, (unsigned)i); b8(&pal, (unsigned)(i * 3) & 0xFF); b8(&pal, (unsigned)(255 - i)); b8(&pal, 0); }
    for (i = 0; i < 256; i++) { b8(&gpal, (unsigned)(i * 5) & 0xFF); b8(&gpal, (unsigned)i); b8(&gpal, 7); b8(&gpal, 0); }
    for (i = 0; i < 32 * 256; i++) b8(&shd, (unsigned)(i * 13 + 5) & 0xFF);
    for (i = 0; i < 65536; i++) b8(&alp, (unsigned)((i >> 8) + (i & 0xFF)) / 2);
    s_nfiles = 0;
    syn("gamedata/sidedata.tdf", make_text(
        "[GENERAL]{baseheight=480;}\n// the sides\n[SIDE0]{name=ARM;nameprefix=ARM;intgaf=ARMINT;commander= ARMCOM ;}\n"
        "[SIDE1]{name=CORE;nameprefix=COR;intgaf=CORINT;commander=CORCOM;}\n"));
    /* every cast of the rule: ARMCOM builds the builder ARMCK, the advbuilder ARMACK and the walker
       ARMPW; ARMCK's menu holds the mex, the geo (G inside its yard), the tower ARMWIN, the wall
       and, through the download fill of ARMDL, the solar; ARMACK's the moho. ARMCK's display name
       is past 31 bytes and holds Windows-1252's euro sign and an undefined byte. */
    syn("units/ARMCOM.fbi", make_text("[UNITINFO]{UnitName=ARMCOM;BMcode=1;MaxVelocity=1.25;Builder=1;WorkerTime=300;"
        "FootprintX=2;FootprintZ=2;BuildTime=50000;}\n"));
    syn("units/ARMCK.fbi", make_text("[UNITINFO]\n{\n\tUnitName=ARMCK;\n\tSide=ARM;\n\tcanstop=1;\n\t/* a comment */\n"
        "\tBMcode=1;MaxVelocity=1.1;Builder=1;WorkerTime=80;FootprintX=2;FootprintZ=2;BuildTime=5597;BuildDistance=40;\n"
        "\tName=Construction KBot;EnglishName=Construction Kbot \x80 with \x81 a name past the cut;\n}\n"));
    syn("units/ARMACK.fbi", make_text("[UNITINFO]{UnitName=ARMACK;BMcode=1;MaxVelocity=1;Builder=1;WorkerTime=180;"
        "FootprintX=2;FootprintZ=2;BuildTime=9000;}\n"));
    syn("units/ARMPW.fbi", make_text("[UNITINFO]{UnitName=ARMPW;BMcode=1;MaxVelocity=2.5;FootprintX=1;FootprintZ=1;BuildTime=1400;"
        "Weapon1=EMG;MinWaterDepth=-10;}\n"));
    syn("units/ARMMEX.fbi", make_text("[UNITINFO]{UnitName=ARMMEX;Side=ARM;FootprintX=2;FootprintZ=2;ExtractsMetal=.001;"
        "BuildTime=1800;}\n"));
    syn("units/ARMMOHO.fbi", make_text("[UNITINFO]{UnitName=ARMMOHO;FootprintX=3;FootprintZ=3;ExtractsMetal=0.003e0;BuildTime=14000;}\n"));
    syn("units/ARMGEO.fbi", make_text("[UNITINFO]{UnitName=ARMGEO;FootprintX=4;FootprintZ=4;YardMap=oooo oGGo oGGo oooo;"
        "EnergyMake=250;EnergyUse=+1;BuildTime=13000;}\n"));
    syn("units/ARMSOLAR.fbi", make_text("[UNITINFO]{UnitName=ARMSOLAR;Side=ARM;canstop=0;FootprintX=5;FootprintZ=5;"
        "EnergyUse=-20;BuildTime=2600;}\n"));
    syn("units/ARMWIN.fbi", make_text("[UNITINFO]{UnitName=ARMWIN;Side=ARM;FootprintX=2;FootprintZ=2;Weapon1=LASER;BuildTime=2000;}\n"));
    syn("units/ARMDRAG.fbi", make_text("[UNITINFO]{UnitName=ARMDRAG;FootprintX=2;FootprintZ=2;TEDClass= fort ;BuildTime=255;}\n"));
    syn("download/armck.tdf", make_text(
        "[MENUENTRY1]{UNITMENU=ARMCK;MENU=3;BUTTON=1;UNITNAME=ARMSOLAR;}\n"
        "[MENUENTRY2]{UNITMENU=ARMCK;MENU=2;BUTTON=99;UNITNAME=ARMMEX;}\n[MENUENTRY3]{MENU=1;}\n"));
    syn("guis/ARMMAIN2.GUI", make_text(GUI_HEAD("ARMMAIN2", "ARMPAN2", "352")
        "[GADGET1]{[COMMON]{id=5;name=KILLS;xpos=14;ypos=13;width=92;height=16;active=1;}text=;}\r\n"));
    syn("guis/ARMGEN.GUI", make_text(GUI_HEAD("ARMGEN", "ARMPAN", "352")
        "[GADGET1]{[COMMON]{id=7;name=ARMFONT;}filename=ARMBUTT;}\r\n"
        GUI_BTN("GADGET2", "ARMORDERS", "3", "4", "58", "18", "16", "0")
        GUI_BTN("GADGET3", "ARMBUILD", "65", "4", "58", "18", "16", "0")
        GUI_BTN("GADGET4", "ARMSTOP", "64", "247", "54", "30", "256", "0")
        "[GADGET5]{[COMMON]{id=1;name=ARMMOVE;xpos=5;ypos=247;width=54;height=30;attribs=256\r\ncolorf=15;active=1;}text=Move;}\r\n"));
    syn("guis/ARMCOM1.GUI", make_text(GUI_HEAD("ARMCOM1", "ARMPAN", "352")
        GUI_BTN("GADGET1", "ARMCK", "0", "27", "64", "64", "32", "4")
        GUI_BTN("GADGET2", "ARMACK", "64", "27", "64", "64", "32", "4")
        GUI_BTN("GADGET3", "ARMPW", "0", "91", "64", "64", "32", "4")));
    syn("guis/ARMACK1.GUI", make_text(GUI_HEAD("ARMACK1", "ARMPAN", "352")
        GUI_BTN("GADGET1", "ARMMOHO", "0", "27", "64", "64", "32", "4")));
    syn("guis/ARMCK0.GUI", make_text(GUI_HEAD("ARMCK0", "ARMPAN", "64")
        GUI_BTN("GADGET1", "ARMSTOP", "4", "4", "54", "30", "0", "0")));
    syn("guis/ARMCK1.GUI", make_text(GUI_HEAD("ARMCK1", "ARMPAN", "352")
        GUI_BTN("GADGET1", "ARMORDERS", "3", "4", "58", "18", "16", "0")
        GUI_BTN("GADGET2", "ARMMEX", "0", "27", "64", "64", "32", "4")
        GUI_BTN("GADGET2", "ARMWIN", "64", "27", "64", "64", "32", "4")
        GUI_BTN("GADGET3", "ARMSHOT", "0", "91", "64", "64", "32", "4")
        GUI_BTN("GADGET3", "IGPATCH", "64", "91", "64", "64", "32", "4")
        GUI_BTN("GADGET3", "ARMGEO", "0", "155", "64", "64", "32", "4")
        GUI_BTN("GADGET3", "ARMDRAG", "64", "155", "64", "64", "32", "4")
        GUI_BTN("GADGET4", "ARMPREV", "8", "222", "44", "16", "0", "0")
        GUI_BTN("GADGET5", "ARMNEXT", "72", "222", "44", "16", "0", "0")
        "[GADGET6]{[COMMON]{id=1;name=ARMSCROLL;xpos=-5;ypos=300;width=200;height=9;attribs=6144;active=0;}}\r\n"));
    syn("guis/ARMCK2.GUI", make_text(""));
    syn("guis/ARMDL.GUI", make_text(GUI_HEAD("ARMDL", "NOPANEL", "352")
        GUI_BTN("GADGET1", "ARMPREV", "8", "222", "44", "16", "0", "0")
        GUI_BTN("GADGET2", "ARMNEXT", "72", "222", "44", "16", "0", "0")
        GUI_BTN("GADGET3", "IGPATCH", "0", "0", "64", "64", "32", "0")
        GUI_BTN("GADGET4", "IGPATCH", "64", "0", "64", "64", "32", "0")
        GUI_BTN("GADGET5", "IGPATCH", "0", "64", "64", "64", "32", "0")));
    syn("anims/commongui.gaf", make_gaf(common, (int)(sizeof common / sizeof *common)));
    syn("anims/ARMCK1.gaf", make_gaf(page, 1));
    syn("anims/ARMINT.gaf", make_gaf(intgaf, 1));
    syn("anims/fx.gaf", make_gaf(fx, 2));
    syn("anims/ARMSOLAR_gadget.gaf", make_gaf(gadget, 1));
    syn("unitpics/ARMWIN.pcx", make_pcx(31, 30));
    syn("palettes/PALETTE.PAL", pal);
    syn("palettes/PALETTE.SHD", shd);
    syn("palettes/PALETTE.ALP", alp);
    syn("palettes/guipal.PAL", gpal);
}

static int write_file(const char* root, const char* rel, const unsigned char* b, uint32_t n)
{
    char path[1024], dir[1024], *sl;
    FILE* f;
    snprintf(path, sizeof path, "%s/%s", root, rel);
    snprintf(dir, sizeof dir, "%s", path);
    if ((sl = strrchr(dir, '/')) != NULL) {
        char cmd[1100];
        *sl = 0;
        snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
        if (system(cmd)) return 0;
    }
    if (!(f = fopen(path, "wb"))) return 0;
    if (n && fwrite(b, 1, n, f) != n) { fclose(f); return 0; }
    return !fclose(f);
}

/* a damaged copy: truncated, flipped, the grammar's characters scattered in, a structural word of a
   GAF set huge, emptied; `*gone` asks for the file to be removed */
static unsigned char* damage_syn(const BUF* b, int is_gaf, uint32_t* out, int* gone)
{
    static const char gram[] = "[]{}=;/*\n\r\t ";
    static const uint32_t huge[] = { 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0xFFFFu, 0x10000u, 0x00FF00FFu };
    const uint32_t mode = rnd() % 7;
    unsigned char* c = (unsigned char*)malloc(b->n + 64);
    uint32_t n = b->n, k;
    if (b->n) memcpy(c, b->b, b->n);
    *gone = 0;
    switch (mode) {
    case 0: n = n ? rnd() % n : 0; break;
    case 1: for (k = 0; n && k < 1 + rnd() % 8; k++) c[rnd() % n] ^= (unsigned char)(1 + rnd() % 255); break;
    case 2: for (k = 0; n && k < 1 + rnd() % 6; k++) c[rnd() % n] = (unsigned char)gram[rnd() % (sizeof gram - 1)]; break;
    case 3:
        if (is_gaf && s_nhot) {
            const uint32_t at = s_hot[rnd() % s_nhot] + 2 * (rnd() % 10);
            if (at + 4 <= n) wr32(c + at, huge[rnd() % (sizeof huge / sizeof *huge)]);
        } else if (n >= 4) wr32(c + (rnd() % (n - 3)), huge[rnd() % (sizeof huge / sizeof *huge)]);
        break;
    case 4: n = 0; break;
    case 5: *gone = 1; break;
    default:
        for (k = 0; n && k < 4; k++) c[rnd() % n] = (unsigned char)rnd();
        if (n) n -= rnd() % (n < 16 ? n : 16);
        break;
    }
    *out = n;
    return c;
}

static WIKIDATA* synth_data(void)
{
    static WIKIDATA d;
    static WIKIENTRY e;
    static WIKIDEF def[3];
    static WIKISCENEUNIT u[3];
    memset(&d, 0, sizeof d);
    memset(&e, 0, sizeof e);
    memset(def, 0, sizeof def);
    memset(u, 0, sizeof u);
    strcpy(def[0].name, "ARMCK");
    strcpy(def[1].name, "ARMSOLAR");
    strcpy(def[2].name, "CORCK");
    def[0].cast = WIKI_CAST_BUILDER;
    def[1].cast = WIKI_CAST_SOLAR;
    def[2].cast = -1;
    u[0].def = 0; u[1].def = 1; u[2].def = 2; u[2].owner = 1;
    e.id = "fuzz";
    e.ndef = 3;
    e.def = def;
    e.scene.nunit = 3;
    e.scene.unit = u;
    d.nentry = 1;
    d.entry = &e;
    return &d;
}

static const char* const SEEDS[] = {
    "[A]{x=1;[COMMON]{id=1;name=A;}}[A]{x=2;x=3;X=4;}",
    "[HEADER]{[COMMON]{id=0;width=128;}panel=P}[B]{v=w;}",
    "/* open comment [a]{b=c;}",
    "k=v; // tail\n[s]{t=u;}}[after]{ignored=1;}",
    "[a]{[b]{[c]{[d]{e=f;}}}}[]{=;}",
    "[x]\n\n\t {\r\n  key = value with spaces ; }\n",
};

/* THE CASTING'S BOUNDS: each casting of the report, read by tagpu_wikidata_casting -- the pack's
   reader, which holds sim.js's checkCasting -- must be accepted and equal the block's, field for
   field. 1 when every side's is. */
static int casting_agrees(const WIKISIDE* side, const char* rep, char* why, size_t nwhy)
{
    char err[200];
    TJDOC* doc = tagpu_json_parse(rep, strlen(rep), err, sizeof err);
    const TJV *list = doc ? tj_get(tagpu_json_root(doc), "casting") : NULL, *v;
    int k = 0, c, ok = 1;
    if (!tj_is(list, TJ_ARR) || list->n != side->layout.nside) {
        snprintf(why, nwhy, "the report's casting is not one a layout side%s%s", doc ? "" : ": ", doc ? "" : err);
        if (doc) tagpu_json_free(doc);
        return 0;
    }
    for (v = list->first; v && ok; v = v->next, k++) {
        WIKICASTING got;
        if (!tagpu_wikidata_casting(v, &got, err, sizeof err)) { snprintf(why, nwhy, "side %d: %s", k, err); ok = 0; break; }
        for (c = 0; c < WIKI_NCAST && ok; c++) {
            const WIKICASTDEF *a = &got.cast[c], *b = &side->casting[k].cast[c];
            ok = a->bound == b->bound && !strcmp(a->type, b->type) && !strcmp(a->title, b->title) && a->fx == b->fx &&
                 a->fz == b->fz && a->build_time == b->build_time && a->worker_time == b->worker_time &&
                 a->build_distance == b->build_distance && a->max_velocity == b->max_velocity && a->extracts == b->extracts &&
                 a->energy_make == b->energy_make && a->canstop == b->canstop && a->vent == b->vent &&
                 !memcmp(a->g, b->g, sizeof a->g) && a->builds == b->builds;
            if (!ok) snprintf(why, nwhy, "side %d's %s read back differs from the block's", k, CAST_NAME[c]);
        }
    }
    tagpu_json_free(doc);
    return ok;
}

static int cmd_fuzz_side(int argc, char** argv)
{
    const char* root = argv[2];
    const int cases = atoi(argv[3]);
    static WIKI3DIN in;
    int i, k, blocks = 0, refused = 0, unnamed = 0, gui_ok = 0, gui_bad = 0, pcx_ok = 0;
    (void)argc;
    s_rng = (uint32_t)strtoul(argv[4], NULL, 10) | 1u;
    synth_install();
    for (k = 0; k < s_nfiles; k++)
        if (!write_file(root, s_files[k].path, s_files[k].body.b, s_files[k].body.n)) { fprintf(stderr, "wikiassettest: cannot write %s\n", s_files[k].path); return 2; }
    snprintf(in.cwd, sizeof in.cwd, "%s", root);
    strcpy(in.names.language, "english");
    strcpy(in.names.units, "units"); strcpy(in.names.anims, "anims"); strcpy(in.names.gui, "guis");
    strcpy(in.names.gamedata, "gamedata"); strcpy(in.names.unitpics, "unitpics"); strcpy(in.names.download, "download");
    strcpy(in.names.commongui, "commongui"); strcpy(in.names.palettes, "palettes"); strcpy(in.names.palette, "PALETTE");
    strcpy(in.names.guipal, "guipal");
    in.data = synth_data();
    /* the whole load, round 0 on the undamaged install */
    for (i = 0; i <= cases; i++) {
        const int pick = i ? (int)(rnd() % (uint32_t)(s_nfiles + 1)) : s_nfiles;
        TAGPU_VFS* vfs;
        const WIKISIDE* side;
        char* rep = NULL;
        char err[200];
        if (pick < s_nfiles) {
            uint32_t n;
            int gone;
            const SYNFILE* f = &s_files[pick];
            unsigned char* c = damage_syn(&f->body, strstr(f->path, ".gaf") != NULL, &n, &gone);
            char path[1024];
            snprintf(path, sizeof path, "%s/%s", root, f->path);
            if (gone) remove(path); else write_file(root, f->path, c, n);
            free(c);
        }
        vfs = tagpu_vfs_open(in.cwd, NULL, 0, in.names.language);
        side = tagpu_wikiside_load(&in, vfs, &rep);
        if (!rep) unnamed++;
        else if (side) {
            blocks++;
            if (!side_frames_inside(side) || !tagpu_wikilayout_check(&side->layout, err, sizeof err) || strcmp(side->report, rep)) {
                fprintf(stderr, "wikiassettest: round %d made a block that breaks its own bounds\n", i);
                return 1;
            }
            if (!casting_agrees(side, rep, err, sizeof err)) {
                fprintf(stderr, "wikiassettest: round %d made a casting the pack's reader refuses or reads otherwise: %s\n", i, err);
                return 1;
            }
        } else if (!strstr(rep, "\"refused\":\"")) unnamed++;
        else {
            const char* r = strstr(rep, "\"refused\":\"") + 11;
            char why[96];
            size_t w = strcspn(r, "\"");
            snprintf(why, sizeof why, "%.*s", (int)(w < 95 ? w : 95), r);
            refused++;
            tally(why);
        }
        if (!i && !side) { fprintf(stderr, "wikiassettest: the undamaged synthetic install is refused: %s\n", rep ? rep : "?"); return 1; }
        if (!i) {
            /* the undamaged install reaches every source a picture has, and the column's art */
            int from[WIKISIDE_FROM_UNITPICS + 1] = { 0 }, s2, g2, panels = 0, nbound = 0;
            for (s2 = 0; s2 < side->layout.nscreen; s2++) {
                panels += side->screen[s2].has_panel;
                for (g2 = 0; g2 < side->layout.screen[s2].ngadget; g2++) from[side->screen[s2].pic[g2].from]++;
            }
            printf("fuzz-side: the undamaged install: %d screens, %d panels; pictures: %d labels, %d page, %d commongui, "
                   "%d fallback, %d download, %d unitpics; panelside %d, %d radlogo, high %d, greyed %d, a %d-row sheet\n",
                   side->layout.nscreen, panels, from[0], from[1], from[2], from[3], from[4], from[5], side->has_panelside[0],
                   side->nradlogo, side->has_high, side->has_greyed, side->sheet_h);
            for (k = 1; k <= WIKISIDE_FROM_UNITPICS; k++)
                if (!from[k]) { fprintf(stderr, "wikiassettest: the synthetic install reaches no picture of source %d\n", k); return 1; }
            if (!side->has_panelside[0] || !side->nradlogo || !side->has_high || !side->has_greyed) {
                fprintf(stderr, "wikiassettest: the synthetic install misses the column's art\n");
                return 1;
            }
            /* and every cast of the rule, its display name cut and converted */
            for (k = 0; k < WIKI_NCAST; k++) nbound += side->casting[0].cast[k].bound;
            printf("fuzz-side: the undamaged install's casting: %d of %d casts bound; the builder %s \"%s\", builds 0x%x; "
                   "the geo's G rows %x %x %x %x\n", nbound, WIKI_NCAST, side->casting[0].cast[0].type,
                   side->casting[0].cast[0].title, side->casting[0].cast[0].builds, side->casting[0].cast[WIKI_CAST_GEO].g[0],
                   side->casting[0].cast[WIKI_CAST_GEO].g[1], side->casting[0].cast[WIKI_CAST_GEO].g[2],
                   side->casting[0].cast[WIKI_CAST_GEO].g[3]);
            if (nbound != WIKI_NCAST || strcmp(side->casting[0].cast[0].title, "Construction Kbot \xE2\x82\xAC with ? a na") ||
                side->casting[0].cast[WIKI_CAST_GEO].g[1] != 6 || side->casting[0].cast[WIKI_CAST_GEO].g[0] ||
                side->side_of_engine[0] != 0 || side->side_of_engine[1] != -1) {
                fprintf(stderr, "wikiassettest: the synthetic install's casting is not the rule's\n");
                return 1;
            }
        }
        free((void*)side);
        free(rep);
        tagpu_vfs_close(vfs);
        if (pick < s_nfiles) write_file(root, s_files[pick].path, s_files[pick].body.b, s_files[pick].body.n);
    }
    printf("fuzz-side: %d loads of the synthetic install, one file damaged each: %d blocks, %d refused, %d unnamed; the refusals:\n",
           cases + 1, blocks, refused, unnamed);
    for (k = 0; k < s_nreason; k++) printf("  %5d  %s\n", s_count[k], s_reason[k]);
    /* the positional reader alone: damaged texts, then pathological ones */
    for (i = 0; i < cases; i++) {
        const int pick = (int)(rnd() % (uint32_t)(s_nfiles + (int)(sizeof SEEDS / sizeof *SEEDS)));
        BUF b = { 0 };
        uint32_t n;
        int gone, sec;
        unsigned char* c;
        char why[200] = "";
        TAGPU_GUIFILE* g;
        if (pick < s_nfiles) bput(&b, s_files[pick].body.b, s_files[pick].body.n);
        else bput(&b, SEEDS[pick - s_nfiles], (uint32_t)strlen(SEEDS[pick - s_nfiles]));
        c = damage_syn(&b, 0, &n, &gone);
        if ((g = tagpu_guifile_parse(c, n, why, sizeof why)) != NULL) {
            /* every section, by a walk of the API alone */
            int stack[256], sp = 0;
            stack[sp++] = 0;
            while (sp) {
                const int at = stack[--sp];
                long long v;
                (void)tagpu_guifile_name(g, at);
                (void)tagpu_guifile_find(g, at, "COMMON");
                tagpu_guifile_num(tagpu_guifile_get(g, at, "id"), &v, NULL);
                (void)tagpu_guifile_get(g, at, "name");
                for (k = 0; k < tagpu_guifile_count(g, at) && sp < 256; k++)
                    if ((sec = tagpu_guifile_child(g, at, k)) >= 0) stack[sp++] = sec;
            }
            gui_ok++;
        } else if (!why[0]) unnamed++;
        else gui_bad++;
        tagpu_guifile_free(g);
        free(c);
        free(b.b);
    }
    {
        /* nesting far deeper than any file, a key count past any file's, a ']' never found */
        static const char* const unit[] = { "[a]{", "k=v;", "[" };
        static const int reps[] = { 100000, 200000, 100000 };
        int u;
        for (u = 0; u < 3; u++) {
            BUF b = { 0 };
            char why[200] = "";
            TAGPU_GUIFILE* g;
            for (k = 0; k < reps[u]; k++) bput(&b, unit[u], (uint32_t)strlen(unit[u]));
            g = tagpu_guifile_parse(b.b, b.n, why, sizeof why);
            if (!g && !why[0]) unnamed++;
            gui_ok += g != NULL;
            gui_bad += g == NULL;
            tagpu_guifile_free(g);
            free(b.b);
        }
    }
    printf("fuzz-side: the positional reader: %d texts parsed, %d refused with a reason\n", gui_ok, gui_bad);
    /* the PCX reader and the number reader on their own */
    for (i = 0; i < cases; i++) {
        BUF b = make_pcx(1 + (int)(rnd() % 70), 1 + (int)(rnd() % 70));
        uint32_t n;
        int gone, w, h;
        unsigned char *c = damage_syn(&b, 0, &n, &gone), *px = NULL;
        char t[24];
        long long v;
        uint32_t bits;
        if (pcx_read(c, n, &w, &h, &px)) pcx_ok++;
        free(px);
        free(c);
        free(b.b);
        for (k = 0; k < (int)sizeof t - 1; k++) t[k] = (char)(" \t+-0123456789xX;"[rnd() % 17]);
        t[rnd() % sizeof t] = 0;
        t[sizeof t - 1] = 0;
        tagpu_guifile_num(t, &v, &bits);
        if (v > TAGPU_GUIFILE_NUMSAT || v < -TAGPU_GUIFILE_NUMSAT) { fprintf(stderr, "wikiassettest: a number past its saturation\n"); return 1; }
    }
    printf("fuzz-side: the PCX reader: %d of %d damaged copies read\n", pcx_ok, cases);
    for (k = 0; k < s_nfiles; k++) free(s_files[k].body.b);
    return unnamed ? 1 : 0;
}

int main(int argc, char** argv)
{
    if (argc >= 4 && !strcmp(argv[1], "load")) return cmd_load(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "side")) return cmd_side(argc, argv);
    if (argc == 5 && !strcmp(argv[1], "fuzz-hpi")) return cmd_fuzz_hpi(argc, argv);
    if (argc == 6 && !strcmp(argv[1], "fuzz-3do")) return cmd_fuzz_3do(argc, argv);
    if (argc == 5 && !strcmp(argv[1], "fuzz-side")) return cmd_fuzz_side(argc, argv);
    fprintf(stderr, "usage: wikiassettest load|side GAMEDIR PAK [--mounts FILE] [--exe EXE] [--language L] [--archive A]\n"
                    "                    [--folders k=v,...]\n"
                    "       wikiassettest fuzz-hpi ARCHIVE N SEED\n"
                    "       wikiassettest fuzz-3do ARCHIVE PATH N SEED\n"
                    "       wikiassettest fuzz-side EMPTYDIR N SEED\n");
    return 2;
}
