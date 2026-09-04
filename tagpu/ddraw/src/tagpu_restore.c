/* tagpu_restore.c — the Classic++ restorer: the unditherer's full model, run
   inside the DLL through Microsoft's ONNX Runtime, once per map, off the game
   and render threads. research/notes/renderers.md 2.5 has the decision and
   the numbers; this header has what the code relies on.

   THE RUNTIME. onnxruntime.dll 1.20.1 for Windows x86. 1.22.1 is the LAST
   version whose NuGet package carries a 32-bit build (1.23 onward is x64/arm64
   only), but it calls std::_Throw_Cpp_error, which Wine 9's built-in msvcp140
   does not implement — standalone under the instance prefix it aborts with
   "unimplemented function", and inside the game the worker thread silently
   never returns from CreateEnv (measured 2026-09-04). 1.20.1 is the last one
   that runs on the built-in runtime, so it is pinned by hash in
   tools/fetch_onnxruntime.sh and lives beside TotalA.exe with its MIT licence
   and notices; a prefix with the native VC++ 2019 runtime could take 1.22.1. It is loaded LAZILY, from the worker
   thread, never from DllMain (loader lock) and never from the render thread
   mid-present. The C API is one exported function that returns a table of
   function pointers, so nothing is linked: GetProcAddress("OrtGetApiBase"),
   then GetApi(ORT_API_VERSION) with the header the same package shipped. A
   32-bit standalone test of this DLL under Wine 9 ran the full model in
   10.5 ms per 32x32 frame single-threaded (2026-09-04); what this module adds
   to that experiment is being INSIDE the game's process.

   THE MODEL. full.onnx: 12 3x3 convolutions, BatchNorm folded, output =
   input - net(input); the graph is Conv/Relu/Sub only, input "rgb" as
   [n,3,h,w] float in 0..1, NCHW. Every tile is 32x32, so a whole map goes
   through as a few dozen batches of identical shape.

   THE PYTHON PATH IS THE SPEC (unditherer/restore.py, infer.py,
   classical.py), reproduced here rule for rule:
     - a frame whose left/right columns and top/bottom rows agree within 12
       levels on average (is_tileable) is WRAP-padded by the model's depth
       (12 px) before the network and cropped after; every other frame is
       zero-padded, which the convolutions do themselves;
     - output * 255, rounded, clipped to 0..255;
     - tiles are opaque, so the colour-key inpaint that unit frames need does
       not arise here (it will, in the unit atlas: renderers.md 2.5).

   THE CACHE. gamedir/tagpu_cache/terr_<crc32 of tiles+palette>_<count>.rgba
   holds the finished RGBA so a map is restored once per install; the crc
   covers the palette because the restored colour depends on it.

   THREADING. One worker thread per job (CreateThread); the runtime, env and
   session are created once under a critical section and shared — Run() is
   thread-safe in onnxruntime. The caller (the render thread) copies the tile
   bytes and palette INTO the job before it starts, polls the state, and reads
   the result buffer only after state == 1. A newer begin() abandons the old
   job: it notices the generation moved between batches and exits without
   publishing. Results stay until the next begin so a GL reset can re-upload. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
/* The header is written for MSVC. Under -std=c99 MinGW hides the single-
   underscore `_stdcall` the header spells its calling convention with, and
   MinGW's sal.h lacks one SAL annotation the header uses. Level the ground
   before including it. */
#ifndef _stdcall
#define _stdcall __stdcall
#endif
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#include "onnxruntime_c_api.h"
#include "crc32.h"
#include "hook.h"          /* real_LoadLibraryA: the fork hooks LoadLibrary (hook=4) */
#include "tagpu_restore.h"

#define TILE_PX     32
#define TILE_BYTES  (TILE_PX * TILE_PX)
#define DEPTH       12                       /* full model: wrap-pad radius (infer.py) */
#define WRAP_PX     (TILE_PX + 2 * DEPTH)    /* 56                                     */
#define BATCH       64
#define TILEABLE_THR 12.0                    /* classical.is_tileable                  */
#define MODEL_FILE  L"full.onnx"
#define CACHE_DIR   "tagpu_cache"
/* palette entries at TA+0x143A7 are 4 bytes: R, G, B, pad (the native pass
   uploads them verbatim as RGBA) */
#define PAL_R 0
#define PAL_G 1
#define PAL_B 2

static void rlog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}
static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

/* ---- the switch ---- */
int tagpu_classicpp_on(void)
{
    static DWORD last = 0; static int on = 0;
    DWORD t = GetTickCount();
    if (last == 0 || t - last > 500) {
        last = t;
        on = GetFileAttributesA("tagpu_classicpp.on") != INVALID_FILE_ATTRIBUTES;
    }
    return on;
}

/* ---- the runtime, loaded once ---- */
static CRITICAL_SECTION s_cs;
static volatile LONG    s_csInit = 0;
static const OrtApi*    s_api;
static OrtEnv*          s_env;
static OrtSession*      s_session;
static OrtMemoryInfo*   s_mem;
static char*            s_outName;
static int              s_rtState = 0;       /* 0 untried, 1 ready, -1 failed */

static void cs_init(void)
{
    if (InterlockedCompareExchange(&s_csInit, 1, 0) == 0) InitializeCriticalSection(&s_cs);
    else while (s_csInit != 2) Sleep(0);
    s_csInit = 2;
}

static int ort_ok(OrtStatus* st, const char* what)
{
    char b[512];
    if (!st) return 1;
    _snprintf(b, sizeof b, "restore: %s failed: %s", what, s_api->GetErrorMessage(st));
    rlog(b);
    s_api->ReleaseStatus(st);
    return 0;
}

static int ensure_runtime(void)     /* s_cs held */
{
    typedef const OrtApiBase* (ORT_API_CALL *PFN_GETBASE)(void);
    char path[MAX_PATH], b[360];
    HMODULE h = NULL;
    PFN_GETBASE getbase;
    const OrtApiBase* base;
    OrtSessionOptions* so = NULL;
    OrtAllocator* alloc = NULL;
    double t0, t1, t2;

    if (s_rtState) return s_rtState > 0;
    t0 = now_ms();
    /* beside TotalA.exe (the game dir) first, then the ordinary search path */
    if (GetModuleFileNameA(NULL, path, sizeof path)) {
        char* p = strrchr(path, '\\');
        if (p && (size_t)(p - path) + 20 < sizeof path) {
            strcpy(p + 1, "onnxruntime.dll");
            _snprintf(b, sizeof b, "restore: loading %s", path); rlog(b);
            h = real_LoadLibraryA(path);
        }
    }
    if (!h) { rlog("restore: loading onnxruntime.dll by name"); h = real_LoadLibraryA("onnxruntime.dll"); }
    if (!h) {
        _snprintf(b, sizeof b, "restore: onnxruntime.dll did not load (error %lu) -- Classic++ terrain stays indexed", GetLastError());
        rlog(b); s_rtState = -1; return 0;
    }
    getbase = (PFN_GETBASE)GetProcAddress(h, "OrtGetApiBase");
    if (!getbase) { rlog("restore: OrtGetApiBase missing"); s_rtState = -1; return 0; }
    base = getbase();
    s_api = base->GetApi(ORT_API_VERSION);
    if (!s_api) {
        _snprintf(b, sizeof b, "restore: onnxruntime %s has no API %d", base->GetVersionString(), ORT_API_VERSION);
        rlog(b); s_rtState = -1; return 0;
    }
    t1 = now_ms();
    _snprintf(b, sizeof b, "restore: onnxruntime %s loaded in %.0f ms; creating the session", base->GetVersionString(), t1 - t0); rlog(b);
    if (!ort_ok(s_api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "tagpu", &s_env), "CreateEnv")) { s_rtState = -1; return 0; }
    rlog("restore: env created");
    if (!ort_ok(s_api->CreateSessionOptions(&so), "CreateSessionOptions")) { s_rtState = -1; return 0; }
    ort_ok(s_api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL), "SetSessionGraphOptimizationLevel");
    /* a fixed intra-op pool for the spike; the runtime's default probes the
       CPU topology, which is one more thing to rule out under Wine */
    ort_ok(s_api->SetIntraOpNumThreads(so, 4), "SetIntraOpNumThreads");
    rlog("restore: session options set; creating the session on full.onnx");
    if (!ort_ok(s_api->CreateSession(s_env, MODEL_FILE, so, &s_session), "CreateSession(full.onnx)")) {
        s_api->ReleaseSessionOptions(so); s_rtState = -1; return 0;
    }
    s_api->ReleaseSessionOptions(so);
    if (!ort_ok(s_api->GetAllocatorWithDefaultOptions(&alloc), "GetAllocatorWithDefaultOptions") ||
        !ort_ok(s_api->SessionGetOutputName(s_session, 0, alloc, &s_outName), "SessionGetOutputName") ||
        !ort_ok(s_api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &s_mem), "CreateCpuMemoryInfo")) {
        s_rtState = -1; return 0;
    }
    t2 = now_ms();
    _snprintf(b, sizeof b, "restore: onnxruntime %s loaded in %.0f ms, session on full.onnx in %.0f ms (api %d, output '%s')",
              base->GetVersionString(), t1 - t0, t2 - t1, ORT_API_VERSION, s_outName);
    rlog(b);
    s_rtState = 1;
    return 1;
}

/* ---- one job ---- */
typedef struct {
    int            gen, count;
    unsigned char* tiles;                /* count * TILE_BYTES                  */
    unsigned char  pal[256 * 4];
    unsigned char* rgba;                 /* count * TILE_BYTES * 4 when done    */
    volatile LONG  state;                /* 0 running, 1 done, -1 failed        */
    HANDLE         thread;
} Job;
static Job*          s_job;              /* the latest job; older ones free themselves */
static volatile LONG s_gen = 0;

static void job_free(Job* j)
{
    if (!j) return;
    if (j->tiles) free(j->tiles);
    if (j->rgba) free(j->rgba);
    if (j->thread) CloseHandle(j->thread);
    free(j);
}

/* classical.is_tileable on the tile's palette colours: mean |left - right|
   and |top - bottom| over the three channels, 0..255 scale, both under 12 */
static int is_tileable(const unsigned char* t, const unsigned char* pal)
{
    double lr = 0.0, tb = 0.0;
    int i, c;
    for (i = 0; i < TILE_PX; i++) {
        const unsigned char* a = pal + t[i * TILE_PX] * 4;
        const unsigned char* b = pal + t[i * TILE_PX + TILE_PX - 1] * 4;
        const unsigned char* u = pal + t[i] * 4;
        const unsigned char* d = pal + t[(TILE_PX - 1) * TILE_PX + i] * 4;
        for (c = 0; c < 3; c++) { lr += abs((int)a[c] - (int)b[c]); tb += abs((int)u[c] - (int)d[c]); }
    }
    lr /= 3.0 * TILE_PX; tb /= 3.0 * TILE_PX;
    return lr < TILEABLE_THR && tb < TILEABLE_THR;
}

/* one tile into slot n of an NCHW batch of HxH frames; wrap pads by DEPTH */
static void fill_tile(float* in, int n, int H, const unsigned char* t, const unsigned char* pal, int wrap)
{
    const int ch[3] = { PAL_R, PAL_G, PAL_B };
    int c, y, x;
    for (c = 0; c < 3; c++) {
        float* pl = in + ((size_t)n * 3 + c) * H * H;
        for (y = 0; y < H; y++) {
            int ty = wrap ? ((y - DEPTH) % TILE_PX + TILE_PX) % TILE_PX : y;
            for (x = 0; x < H; x++) {
                int tx = wrap ? ((x - DEPTH) % TILE_PX + TILE_PX) % TILE_PX : x;
                pl[y * H + x] = (float)pal[t[ty * TILE_PX + tx] * 4 + ch[c]] * (1.0f / 255.0f);
            }
        }
    }
}

/* slot n of an NCHW output batch of HxH frames -> one tile's RGBA (centre crop when wrapped) */
static void unpack_tile(const float* out, int n, int H, unsigned char* rgba, int wrap)
{
    int off = wrap ? DEPTH : 0;
    int c, y, x;
    for (y = 0; y < TILE_PX; y++)
        for (x = 0; x < TILE_PX; x++) {
            unsigned char* d = rgba + ((size_t)y * TILE_PX + x) * 4;
            for (c = 0; c < 3; c++) {
                float v = out[((size_t)n * 3 + c) * H * H + (size_t)(y + off) * H + (x + off)];
                float r = floorf(v * 255.0f + 0.5f);          /* np.rint then clip */
                d[c] = (unsigned char)(r < 0.0f ? 0 : r > 255.0f ? 255 : r);
            }
            d[3] = 255;
        }
}

static int run_batch(const float* in, int n, int H, float* out)
{
    int64_t shape[4] = { n, 3, H, H };
    OrtValue* iv = NULL; OrtValue* ov = NULL;
    const char* in_names[1] = { "rgb" };
    const char* out_names[1] = { s_outName };
    float* od = NULL;
    size_t bytes = (size_t)n * 3 * H * H * sizeof(float);
    if (!ort_ok(s_api->CreateTensorWithDataAsOrtValue(s_mem, (void*)in, bytes, shape, 4,
                                                      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &iv),
                "CreateTensorWithDataAsOrtValue")) return 0;
    if (!ort_ok(s_api->Run(s_session, NULL, in_names, (const OrtValue* const*)&iv, 1, out_names, 1, &ov), "Run")) {
        s_api->ReleaseValue(iv); return 0;
    }
    if (!ort_ok(s_api->GetTensorMutableData(ov, (void**)&od), "GetTensorMutableData")) {
        s_api->ReleaseValue(ov); s_api->ReleaseValue(iv); return 0;
    }
    memcpy(out, od, bytes);
    s_api->ReleaseValue(ov);
    s_api->ReleaseValue(iv);
    return 1;
}

static void cache_name(const Job* j, char* out, size_t n)
{
    unsigned long crc = Crc32_ComputeBuf(0, j->tiles, (size_t)j->count * TILE_BYTES);
    crc = Crc32_ComputeBuf(crc, j->pal, sizeof j->pal);
    _snprintf(out, n, CACHE_DIR "\\terr_%08lx_%d.rgba", crc, j->count);
}

static int cache_read(Job* j)
{
    char name[128]; FILE* f; long sz; size_t want = (size_t)j->count * TILE_BYTES * 4;
    cache_name(j, name, sizeof name);
    f = fopen(name, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if ((size_t)sz != want) { fclose(f); return 0; }
    j->rgba = (unsigned char*)malloc(want);
    if (!j->rgba || fread(j->rgba, 1, want, f) != want) { fclose(f); free(j->rgba); j->rgba = NULL; return 0; }
    fclose(f);
    return 1;
}

static void cache_write(const Job* j)
{
    char name[128], tmp[136]; FILE* f; size_t n = (size_t)j->count * TILE_BYTES * 4;
    CreateDirectoryA(CACHE_DIR, NULL);
    cache_name(j, name, sizeof name);
    _snprintf(tmp, sizeof tmp, "%s.tmp", name);
    f = fopen(tmp, "wb");
    if (!f) return;
    if (fwrite(j->rgba, 1, n, f) != n) { fclose(f); DeleteFileA(tmp); return; }
    fclose(f);
    MoveFileExA(tmp, name, MOVEFILE_REPLACE_EXISTING);
}

static DWORD WINAPI job_main(LPVOID p)
{
    Job* j = (Job*)p;
    char b[256];
    double t0 = now_ms(), tr;
    int ok, i, pass, nwrap = 0, nb = 0;
    unsigned char* wrap = NULL;
    float *in = NULL, *out = NULL;
    int idx[BATCH];

    EnterCriticalSection(&s_cs);
    ok = ensure_runtime();
    LeaveCriticalSection(&s_cs);
    if (!ok) { InterlockedExchange(&j->state, -1); return 0; }
    tr = now_ms();

    if (cache_read(j)) {
        _snprintf(b, sizeof b, "restore: terrain gen %d: %d tiles from cache in %.0f ms", j->gen, j->count, now_ms() - tr);
        rlog(b);
        InterlockedExchange(&j->state, 1);
        return 0;
    }

    j->rgba = (unsigned char*)malloc((size_t)j->count * TILE_BYTES * 4);
    wrap = (unsigned char*)malloc((size_t)j->count);
    in  = (float*)malloc((size_t)BATCH * 3 * WRAP_PX * WRAP_PX * sizeof(float));
    out = (float*)malloc((size_t)BATCH * 3 * WRAP_PX * WRAP_PX * sizeof(float));
    if (!j->rgba || !wrap || !in || !out) {
        rlog("restore: out of memory"); free(wrap); free(in); free(out);
        InterlockedExchange(&j->state, -1); return 0;
    }
    for (i = 0; i < j->count; i++) { wrap[i] = (unsigned char)is_tileable(j->tiles + (size_t)i * TILE_BYTES, j->pal); nwrap += wrap[i]; }

    /* two passes, one per input shape: plain 32x32, then wrap-padded 56x56 */
    for (pass = 0; pass < 2 && ok; pass++) {
        int H = pass ? WRAP_PX : TILE_PX, n = 0;
        for (i = 0; i <= j->count && ok; i++) {
            if (i < j->count && wrap[i] == pass) {
                fill_tile(in, n, H, j->tiles + (size_t)i * TILE_BYTES, j->pal, pass);
                idx[n++] = i;
            }
            if (n == BATCH || (i == j->count && n > 0)) {
                int k;
                if (j->gen != s_gen) { ok = 0; break; }          /* abandoned: a newer map */
                if (!run_batch(in, n, H, out)) { ok = 0; break; }
                for (k = 0; k < n; k++) unpack_tile(out, k, H, j->rgba + (size_t)idx[k] * TILE_BYTES * 4, pass);
                nb++; n = 0;
            }
        }
    }
    free(wrap); free(in); free(out);
    if (!ok) {
        if (j->gen != s_gen) { _snprintf(b, sizeof b, "restore: terrain gen %d abandoned (map changed)", j->gen); rlog(b); }
        InterlockedExchange(&j->state, -1);
        return 0;
    }
    cache_write(j);
    _snprintf(b, sizeof b, "restore: terrain gen %d: %d tiles (%d wrap-padded) in %d batches, %.0f ms model (%.0f ms total), cached",
              j->gen, j->count, nwrap, nb, now_ms() - tr, now_ms() - t0);
    rlog(b);
    InterlockedExchange(&j->state, 1);
    return 0;
}

int tagpu_restore_terrain_begin(const unsigned char* tiles, int count, const unsigned char* pal)
{
    Job* j;
    char b[160];
    if (!tiles || !pal || count <= 0) return 0;
    cs_init();
    j = (Job*)calloc(1, sizeof *j);
    if (!j) return 0;
    j->count = count;
    j->tiles = (unsigned char*)malloc((size_t)count * TILE_BYTES);
    if (!j->tiles) { free(j); return 0; }
    memcpy(j->tiles, tiles, (size_t)count * TILE_BYTES);
    memcpy(j->pal, pal, sizeof j->pal);
    j->gen = (int)InterlockedIncrement(&s_gen);
    /* the previous job, if any: it exits on its own when it sees s_gen move;
       its buffers go when the thread has gone */
    if (s_job) {
        Job* old = s_job;
        if (old->thread && WaitForSingleObject(old->thread, 0) == WAIT_OBJECT_0) job_free(old);
        else if (!old->thread) job_free(old);
        else { /* still running: leak-free later — a finished old thread frees nothing itself, so park it */
            static Job* s_park; if (s_park) { WaitForSingleObject(s_park->thread, INFINITE); job_free(s_park); } s_park = old; }
    }
    s_job = j;
    j->thread = CreateThread(NULL, 0, job_main, j, 0, NULL);
    if (!j->thread) { rlog("restore: CreateThread failed"); s_job = NULL; job_free(j); return 0; }
    _snprintf(b, sizeof b, "restore: terrain gen %d started: %d tiles (%d KB)", j->gen, count, (count * TILE_BYTES) >> 10);
    rlog(b);
    return j->gen;
}

int tagpu_restore_state(int gen)
{
    if (!s_job || s_job->gen != gen) return -1;
    return (int)s_job->state;
}

int tagpu_restore_terrain_result(int gen, const unsigned char** rgba, int* count)
{
    if (!s_job || s_job->gen != gen || s_job->state != 1 || !s_job->rgba) return 0;
    *rgba = s_job->rgba; *count = s_job->count;
    return 1;
}
