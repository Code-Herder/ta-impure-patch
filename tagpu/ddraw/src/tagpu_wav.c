/* tagpu_wav.c -- see tagpu_wav.h */
#include "tagpu_wav.h"

#include <stdio.h>
#include <string.h>

static uint32_t u32(const unsigned char* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned u16(const unsigned char* p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }

static int refuse(char* why, size_t nwhy, const char* text)
{
    if (why && nwhy) snprintf(why, nwhy, "%s", text);
    return 0;
}

int tagpu_wav_parse(const unsigned char* blob, size_t n, TAGPU_WAV* out, char* why, size_t nwhy)
{
    size_t end, at = 12;
    int fmt = 0;
    memset(out, 0, sizeof *out);
    if (!blob || n < 12 || memcmp(blob, "RIFF", 4) || memcmp(blob + 8, "WAVE", 4)) return refuse(why, nwhy, "not a RIFF WAVE file");
    /* the RIFF size, cut to the file: what lies past it is not the file's */
    end = (size_t)u32(blob + 4) < n - 8 ? 8 + (size_t)u32(blob + 4) : n;
    while (at + 8 <= end) {
        const unsigned char* c = blob + at;
        const size_t body = at + 8, len = u32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            unsigned tag, align;
            if (len < 16 || end - body < 16) return refuse(why, nwhy, "a short fmt chunk");
            tag = u16(blob + body);
            out->channels = (int)u16(blob + body + 2);
            out->rate = (int)(u32(blob + body + 4) > 192000u ? 0 : u32(blob + body + 4));
            align = u16(blob + body + 12);
            out->bits = (int)u16(blob + body + 14);
            if (tag != 1) {
                if (why && nwhy) snprintf(why, nwhy, "format tag %u, not PCM", tag);
                return 0;
            }
            if ((out->channels != 1 && out->channels != 2) || (out->bits != 8 && out->bits != 16) || out->rate < 1000 ||
                align != (unsigned)(out->channels * out->bits / 8)) {
                if (why && nwhy)
                    snprintf(why, nwhy, "%d channels, %d bits, %d Hz, align %u", out->channels, out->bits, out->rate, align);
                return 0;
            }
            out->align = (int)align;
            fmt = 1;
        } else if (!memcmp(c, "data", 4)) {
            const size_t have = len < end - body ? len : end - body;
            if (!fmt) return refuse(why, nwhy, "data before fmt");
            out->bytes = (uint32_t)(have - have % (size_t)out->align);
            if (!out->bytes) return refuse(why, nwhy, "no whole frame");
            out->data = blob + body;
            return 1;
        }
        /* a chunk longer than what is left ends the walk; the pad byte is skipped only within it */
        if (len > end - body) break;
        at = body + len + (len & 1);
    }
    return refuse(why, nwhy, "no data chunk");
}
