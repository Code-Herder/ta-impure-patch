#ifndef TAGPU_WAV_H
#define TAGPU_WAV_H
/* tagpu_wav -- a RIFF WAVE file's PCM samples, read for the in-game wiki's sound (tagpu_wikiasset.c
   loads the WAVs, tagpu_wikisound.c plays them). Its Python twin is tools/wiki/gfx.py `wav_pcm`,
   which decides what the mockup's packs carry by the same rule.

   THE RULE: `RIFF`, a size, `WAVE`, then chunks, each a four-byte id, a 32-bit length and that many
   bytes padded to an even count. The `fmt ` chunk must be PCM (format tag 1) with 1 or 2 channels,
   8 or 16 bits, a rate of 1000..192000 Hz and a block align of channels x bits / 8; the first
   `data` chunk after it is the samples, cut to whole frames, at least 4 bytes of them
   (DSBSIZE_MIN: DirectSound makes no smaller buffer). Any other format -- ADPCM, float,
   anything DirectSound would need a codec for -- is refused, never converted.

   BOUNDED BY LENGTH: the RIFF size is cut to the file, every chunk length is compared with what is
   left of it before anything is read past its header, and the walk ends at the first chunk that
   does not fit, so a hostile length addresses nothing outside the `n` bytes given. No allocation,
   no engine memory, no Win32: any thread. tests/wavtest.c feeds it truncated, oversized and
   non-PCM files. */
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int                  channels, bits, rate, align;   /* align: bytes a frame */
    const unsigned char* data;                          /* inside the bytes given */
    uint32_t             bytes;                         /* whole frames only, > 0 */
} TAGPU_WAV;

/* 1 with `out` filled, pointing into `blob`, when the `n` bytes are a WAV the rule accepts; 0 with
   the reason in `why` otherwise. */
int tagpu_wav_parse(const unsigned char* blob, size_t n, TAGPU_WAV* out, char* why, size_t nwhy);

#endif
