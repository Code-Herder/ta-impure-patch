/* tagpu_modpak.c -- Contract: tagpu_modpak.h. */
#include <stdlib.h>
#include <string.h>
#include "tagpu_modpak.h"

/* ---- SHA-256 (FIPS 180-4) ---------------------------------------------------------------------- */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(MODSHA* s, const uint8_t* p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++, p += 4)
        w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void modsha_init(MODSHA* s)
{
    static const uint32_t H0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, H0, sizeof H0);
    s->total = 0;
    s->nbuf = 0;
}

void modsha_add(MODSHA* s, const void* data, size_t n)
{
    const uint8_t* p = data;
    s->total += n;
    if (s->nbuf) {
        size_t take = 64 - s->nbuf < n ? 64 - s->nbuf : n;
        memcpy(s->buf + s->nbuf, p, take);
        s->nbuf += (uint32_t)take; p += take; n -= take;
        if (s->nbuf < 64) return;
        block(s, s->buf);
        s->nbuf = 0;
    }
    for (; n >= 64; p += 64, n -= 64) block(s, p);
    memcpy(s->buf, p, n);
    s->nbuf = (uint32_t)n;
}

void modsha_end(MODSHA* s, uint8_t out[32])
{
    uint64_t bits = s->total * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    int i;
    modsha_add(s, &pad, 1);
    while (s->nbuf != 56) modsha_add(s, &zero, 1);
    for (i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    modsha_add(s, len, 8);
    for (i = 0; i < 32; i++) out[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

/* ---- the pack ---------------------------------------------------------------------------------- */

static uint32_t u32(const uint8_t* p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#define NEED(k) do { if ((size_t)(k) > n - at) goto bad; } while (0)

int tagpu_modpak_read(const uint8_t* b, size_t n, MODPAK* p)
{
    size_t at = 0;
    uint32_t i, j;
    memset(p, 0, sizeof *p);
    NEED(4 + 4 + 5 * 4 + 32);
    if (memcmp(b, "MPAT", 4) || u32(b + 4) != 1) goto bad;
    p->nxors = u32(b + 8); p->nmods = u32(b + 12); p->nknown = u32(b + 16);
    p->nmask = u32(b + 20); p->noriginals = u32(b + 24);
    p->stock_key = b + 28;
    at = 60;
    /* every count is bounded by what its rows would take, so an allocation is never larger than the pack */
    if (p->nxors > n / 4 || p->nmods > n / 37 || p->nknown > n / 37 || p->nmask > n / 8) goto bad;
    p->xors = calloc(p->nxors + 1, sizeof *p->xors);
    p->mods = calloc(p->nmods + 1, sizeof *p->mods);
    p->known = calloc(p->nknown + 1, sizeof *p->known);
    p->mask = calloc(p->nmask + 1, sizeof *p->mask);
    if (!p->xors || !p->mods || !p->known || !p->mask) goto bad;
    for (i = 0; i < p->nxors; i++) {
        NEED(4);
        p->xors[i].nruns = u32(b + at);
        at += 4;
        p->xors[i].at = b + at;
        for (j = 0; j < p->xors[i].nruns; j++) {
            size_t len;
            NEED(6);
            len = b[at + 4] | (size_t)b[at + 5] << 8;
            at += 6;
            NEED(len);
            at += len;
        }
    }
    for (i = 0; i < p->nmods; i++) {
        size_t ln;
        NEED(1);
        ln = b[at++];
        if (!ln || ln >= sizeof p->mods[i].id) goto bad;
        NEED(ln + 32 + 4 + 1);
        memcpy(p->mods[i].id, b + at, ln);
        at += ln;
        p->mods[i].hash = b + at;
        at += 32;
        p->mods[i].xor_ = u32(b + at);
        at += 4;
        p->mods[i].loader = b[at++];
        if (p->mods[i].xor_ >= p->nxors) goto bad;
    }
    for (i = 0; i < p->nknown; i++) {
        NEED(1 + 32 + 4);
        p->known[i].kind = b[at];
        p->known[i].key = b + at + 1;
        p->known[i].xor_ = (int32_t)u32(b + at + 33);
        at += 37;
        if (p->known[i].xor_ >= (int32_t)p->nxors || p->known[i].xor_ < -1) goto bad;
        if (!p->known[i].kind && p->known[i].xor_ < 0) goto bad;
    }
    for (i = 0; i < p->nmask; i++) {
        NEED(8);
        p->mask[i].va = u32(b + at);
        p->mask[i].len = u32(b + at + 4);
        at += 8;
        if (i && p->mask[i].va < p->mask[i - 1].va + p->mask[i - 1].len) goto bad;
    }
    NEED((size_t)p->noriginals * 32);
    if (p->noriginals > n / 32) goto bad;
    p->originals = b + at;
    at += (size_t)p->noriginals * 32;
    if (at != n) goto bad;
    return 1;
bad:
    tagpu_modpak_free(p);
    return 0;
}

void tagpu_modpak_free(MODPAK* p)
{
    free(p->xors); free(p->mods); free(p->known); free(p->mask);
    memset(p, 0, sizeof *p);
}

const MODPATCH* tagpu_modpak_mod(const MODPAK* p, const char* id)
{
    uint32_t i;
    for (i = 0; i < p->nmods; i++) if (!strcmp(p->mods[i].id, id)) return &p->mods[i];
    return NULL;
}

const MODKNOWN* tagpu_modpak_known(const MODPAK* p, int kind, const uint8_t key[32])
{
    uint32_t i;
    for (i = 0; i < p->nknown; i++)
        if (p->known[i].kind == kind && !memcmp(p->known[i].key, key, 32)) return &p->known[i];
    return NULL;
}

int tagpu_modpak_is_original(const MODPAK* p, const uint8_t sha[32])
{
    uint32_t i;
    for (i = 0; i < p->noriginals; i++) if (!memcmp(p->originals + 32 * (size_t)i, sha, 32)) return 1;
    return 0;
}

/* ---- the exe ----------------------------------------------------------------------------------- */

int tagpu_modpak_file_key(const uint8_t* d, size_t n, int is_exe, uint8_t out[32])
{
    MODSHA s;
    modsha_init(&s);
    if (is_exe) {
        static const uint8_t zero[4] = { 0, 0, 0, 0 };
        uint32_t pe;
        if (n < 0x40) return 0;
        pe = u32(d + 0x3C);
        if (pe > n || n - pe < 24 + 68) return 0;
        modsha_add(&s, d, pe + 22);
        modsha_add(&s, zero, 2);
        modsha_add(&s, d + pe + 24, 64);
        modsha_add(&s, zero, 4);
        modsha_add(&s, d + pe + 24 + 68, n - (pe + 24 + 68));
    } else {
        modsha_add(&s, d, n);
    }
    modsha_end(&s, out);
    return 1;
}

int tagpu_modimg_from_exe(MODIMG* img, const uint8_t* d, size_t n)
{
    uint32_t pe, base, nsec, opt, i;
    memset(img, 0, sizeof *img);
    if (n < 0x40) return 0;
    pe = u32(d + 0x3C);
    if (pe > n || n - pe < 24 + 32) return 0;
    nsec = d[pe + 6] | (uint32_t)d[pe + 7] << 8;
    opt = d[pe + 20] | (uint32_t)d[pe + 21] << 8;
    base = u32(d + pe + 24 + 28);
    if ((size_t)pe + 24 + opt + (size_t)nsec * 40 > n) return 0;
    for (i = 0; i < nsec; i++) {
        const uint8_t* s = d + pe + 24 + opt + i * 40;
        uint32_t vaddr = u32(s + 12), rsize = u32(s + 16), raddr = u32(s + 20);
        char name[9];
        memcpy(name, s, 8);
        name[8] = 0;
        if (!strcmp(name, ".rsrc") || !rsize) continue;
        if (raddr > n || rsize > n - raddr || img->nsec == MODPAK_MAXSEC) goto bad;
        img->sec[img->nsec].va = base + vaddr;
        img->sec[img->nsec].len = rsize;
        img->sec[img->nsec].raw = raddr;
        if (!(img->sec[img->nsec].bytes = malloc(rsize))) goto bad;
        memcpy(img->sec[img->nsec].bytes, d + raddr, rsize);
        img->nsec++;
    }
    return 1;
bad:
    tagpu_modimg_free(img);
    return 0;
}

void tagpu_modimg_free(MODIMG* img)
{
    int i;
    for (i = 0; i < img->nsec; i++) free(img->sec[i].bytes);
    memset(img, 0, sizeof *img);
}

int tagpu_modimg_xor(MODIMG* img, const MODXOR* x)
{
    const uint8_t* p = x->at;
    uint32_t r;
    for (r = 0; r < x->nruns; r++) {
        uint32_t va = u32(p), len = p[4] | (uint32_t)p[5] << 8, i;
        int s;
        p += 6;
        for (s = 0; s < img->nsec; s++)
            if (va >= img->sec[s].va && va - img->sec[s].va < img->sec[s].len &&
                len <= img->sec[s].len - (va - img->sec[s].va)) break;
        if (s == img->nsec) return 0;
        for (i = 0; i < len; i++) img->sec[s].bytes[va - img->sec[s].va + i] ^= p[i];
        p += len;
    }
    return 1;
}

void tagpu_modimg_hash(const MODIMG* img, const MODRANGE* mask, uint32_t nmask, uint8_t out[32])
{
    static const uint8_t zero[256];
    MODSHA h;
    int s;
    modsha_init(&h);
    for (s = 0; s < img->nsec; s++) {
        const MODSEC* c = &img->sec[s];
        uint32_t at = c->va, end = c->va + c->len, m;
        for (m = 0; m < nmask && at < end; m++) {
            uint32_t from = mask[m].va, to = mask[m].va + mask[m].len;
            if (to <= at) continue;
            if (from >= end) break;
            if (from > at) { modsha_add(&h, c->bytes + (at - c->va), from - at); at = from; }
            to = to < end ? to : end;
            while (at < to) {
                uint32_t k = to - at < sizeof zero ? to - at : (uint32_t)sizeof zero;
                modsha_add(&h, zero, k);
                at += k;
            }
        }
        if (at < end) modsha_add(&h, c->bytes + (at - c->va), end - at);
    }
    modsha_end(&h, out);
}
