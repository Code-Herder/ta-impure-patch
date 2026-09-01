/* tagpu_hires.c — G12d hi-res model experiment: the REPLACEMENT-MESH SLOT.

   If gamedir/hires/<defname>.obj exists (lowercase unit def name, e.g.
   armsolar.obj), the native pass renders THAT mesh for the unit type instead
   of the engine's 3DO — proving the G11 replacement slot end-to-end: file-
   driven geometry, hot reload (mtime watched), engine anchor + body yaw, our
   shading/palette pipeline. Whole-model only (piece-wise COB pose = G11).

   OBJ subset: `v x y z` (model units = game px; +y up, +z north — same axes
   as the 3DO model space, projected sx=+x, sy=-z-y/2), `f a b c [d]`
   (1-based, quads split), `c <idx>` sets the palette colour index (0..255)
   for subsequent faces (default 0xB4, metal grey). */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_hires.h"

#define MAXV 8192
#define MAXT 16384

typedef struct {
    char     name[32];
    FILETIME mtime;
    int      ntri;
    float    (*pos)[9];     /* per tri: 3 verts x xyz */
    unsigned char* col;     /* per tri: palette index */
    int      valid;
} HMesh;

static HMesh s_mesh[8];
static int   s_nmesh = 0;

static void hlog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static int load_obj(HMesh* m, const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    static float vx[MAXV][3];
    int nv = 0, nt = 0;
    float (*tris)[9] = malloc(sizeof(float) * 9 * MAXT);
    unsigned char* cols = malloc(MAXT);
    unsigned char curc = 0xB4;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'v' && line[1] == ' ') {
            if (nv < MAXV &&
                sscanf(line + 2, "%f %f %f", &vx[nv][0], &vx[nv][1], &vx[nv][2]) == 3)
                nv++;
        } else if (line[0] == 'c' && line[1] == ' ') {
            int c = atoi(line + 2);
            if (c >= 0 && c <= 255) curc = (unsigned char)c;
        } else if (line[0] == 'f' && line[1] == ' ') {
            int a = 0, b = 0, c = 0, d = 0;
            int n = sscanf(line + 2, "%d %d %d %d", &a, &b, &c, &d);
            if (n >= 3 && a >= 1 && b >= 1 && c >= 1 &&
                a <= nv && b <= nv && c <= nv && nt < MAXT) {
                int idx[6] = { a - 1, b - 1, c - 1, a - 1, c - 1, d - 1 };
                int k, t, ntri = (n == 4 && d >= 1 && d <= nv) ? 2 : 1;
                for (t = 0; t < ntri && nt < MAXT; t++) {
                    for (k = 0; k < 3; k++) {
                        int vi = idx[t * 3 + k];
                        tris[nt][k*3+0] = vx[vi][0];
                        tris[nt][k*3+1] = vx[vi][1];
                        tris[nt][k*3+2] = vx[vi][2];
                    }
                    cols[nt] = curc;
                    nt++;
                }
            }
        }
    }
    fclose(f);
    if (nt == 0) { free(tris); free(cols); return 0; }
    free(m->pos); free(m->col);
    m->pos = realloc(tris, sizeof(float) * 9 * nt);
    m->col = cols;
    m->ntri = nt;
    { char b[128]; _snprintf(b, sizeof b, "hires: %s loaded, %d tris", path, nt); hlog(b); }
    return 1;
}

/* returns the mesh for a (lowercased) def name, or NULL. Checks mtime each
   call it is asked to (cheap: caller rate-limits via its 30-frame recheck). */
const void* tagpu_hires_mesh(const char* defname)
{
    char path[96];
    int i;
    HMesh* m = NULL;
    for (i = 0; i < s_nmesh; i++)
        if (!lstrcmpiA(s_mesh[i].name, defname)) { m = &s_mesh[i]; break; }
    if (!m) {
        if (s_nmesh >= 8) return NULL;
        m = &s_mesh[s_nmesh];
        memset(m, 0, sizeof *m);
        lstrcpynA(m->name, defname, sizeof m->name);
        s_nmesh++;
    }
    _snprintf(path, sizeof path, "hires\\%s.obj", m->name);
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
            m->valid = 0;
            return NULL;
        }
        if (CompareFileTime(&fad.ftLastWriteTime, &m->mtime) != 0) {
            m->mtime = fad.ftLastWriteTime;
            m->valid = load_obj(m, path);
        }
    }
    return m->valid ? (const void*)m : NULL;
}

int tagpu_hires_ntri(const void* mesh) { return ((const HMesh*)mesh)->ntri; }
const float* tagpu_hires_tri(const void* mesh, int i) { return ((const HMesh*)mesh)->pos[i]; }
int tagpu_hires_col(const void* mesh, int i) { return ((const HMesh*)mesh)->col[i]; }
