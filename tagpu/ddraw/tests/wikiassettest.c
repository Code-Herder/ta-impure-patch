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
         The load's time and the arena's size go to stderr.
     wikiassettest fuzz-hpi ARCHIVE N SEED
         N copies of ARCHIVE, each truncated, or with bytes flipped (half of them in its header and
         directory), or both, each opened from memory and every file of it read whole; a line per
         reason, and exit 1 when any refusal is not named.
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

static int cmd_load(int argc, char** argv)
{
    const char *dir = argv[2], *pak = argv[3], *mounts = NULL, *exe = NULL, *lang = "english";
    char exepath[1024], first[64], arg[16];
    static WIKI3DIN in;
    unsigned char *eb, *pb, *arena;
    uint32_t en, pn;
    WIKIDATA* data;
    char err[256];
    const WIKI3D* w;
    struct timespec t0, t1;
    int i;
    struct { uint32_t va; char* dst; } names[] = {
        { 0x503920u, in.names.units }, { 0x503720u, in.names.textures }, { 0x504734u, in.names.logos },
        { 0x503704u, in.names.logos_gaf }, { 0x5036E4u, in.names.objects3d }, { 0x503740u, in.names.scripts },
        { 0x502E30u, in.names.anims }, { 0x502C7Cu, in.names.features } };
    for (i = 4; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--mounts")) mounts = argv[i + 1];
        else if (!strcmp(argv[i], "--exe")) exe = argv[i + 1];
        else if (!strcmp(argv[i], "--language")) lang = argv[i + 1];
        else { fprintf(stderr, "wikiassettest: unknown option %s\n", argv[i]); return 2; }
    }
    if (!exe) { snprintf(exepath, sizeof exepath, "%s/TotalA.exe", dir); exe = exepath; }
    if (!(eb = slurp(exe, &en))) { fprintf(stderr, "wikiassettest: cannot read %s\n", exe); return 2; }
    for (i = 0; i < (int)(sizeof names / sizeof *names); i++)
        if (!exe_str(eb, en, names[i].va, names[i].dst, WIKI3D_NAMEMAX)) { fprintf(stderr, "wikiassettest: no folder name at 0x%X\n", names[i].va); return 2; }
    if (!exe_str(eb, en, 0x5028CCu, first, sizeof first) || !exe_str(eb, en, 0x5028D8u, arg, sizeof arg)) {
        fprintf(stderr, "wikiassettest: no archive pattern in %s\n", exe);
        return 2;
    }
    free(eb);
    if (strstr(first, "%s")) {
        char t[64];
        snprintf(t, sizeof t, first, arg);
        strcpy(first, t);
    }
    snprintf(in.names.language, sizeof in.names.language, "%s", lang);
    snprintf(in.cwd, sizeof in.cwd, "%s", dir);
    if (!(mounts ? mount_from(dir, mounts, &in) : mount(dir, first, &in))) { fprintf(stderr, "wikiassettest: no mount list\n"); return 2; }
    if (!(pb = slurp(pak, &pn))) { fprintf(stderr, "wikiassettest: cannot read %s\n", pak); return 2; }
    if (!(data = tagpu_wikidata_load(pb, pn, err, sizeof err))) { fprintf(stderr, "wikiassettest: %s\n", err); return 2; }
    in.data = data;
    in.pak = pb;
    in.npak = pn;
    if (!(arena = (unsigned char*)calloc(1, ARENA_CAP))) return 2;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    w = tagpu_wiki3d_load(&in, arena, STAND_IN_BASE, ARENA_CAP, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (!w) { fprintf(stderr, "wikiassettest: the load published nothing\n"); return 1; }
    fputs(w->report, stdout);
    fprintf(stderr, "wikiassettest: loaded in %.1f ms - arena %lu bytes; %d mounts\n",
            (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6, (unsigned long)w->bytes, in.narc);
    free(arena);
    tagpu_wikidata_free(data);
    free(pb);
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

static int cmd_fuzz_hpi(int argc, char** argv)
{
    uint32_t n, m, hot;
    unsigned char* b;
    int cases = atoi(argv[3]), i, clean = 0;
    s_rng = (uint32_t)strtoul(argv[4], NULL, 10) | 1u;
    (void)argc;
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
            if (sz > TAGPU_HPI_FILEMAX) { tally("a file past the reader's bound"); ok = 0; continue; }
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
                unit_hides(&L.a, NULL, 0, md.root, bits, hw, sizeof hw);   /* both piece walks */
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

int main(int argc, char** argv)
{
    if (argc >= 4 && !strcmp(argv[1], "load")) return cmd_load(argc, argv);
    if (argc == 5 && !strcmp(argv[1], "fuzz-hpi")) return cmd_fuzz_hpi(argc, argv);
    if (argc == 6 && !strcmp(argv[1], "fuzz-3do")) return cmd_fuzz_3do(argc, argv);
    fprintf(stderr, "usage: wikiassettest load GAMEDIR PAK [--mounts FILE] [--exe EXE] [--language L]\n"
                    "       wikiassettest fuzz-hpi ARCHIVE N SEED\n"
                    "       wikiassettest fuzz-3do ARCHIVE PATH N SEED\n");
    return 2;
}
