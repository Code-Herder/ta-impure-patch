#ifndef TAGPU_RECDIFF_H
#define TAGPU_RECDIFF_H
/* tagpu_recdiff.h -- the demo recorder's byte comparison: where two buffers first differ.

   The capture asks it, every iteration, of each unit record and mover it saw live against the copy
   the last capture read, of every static declared table against its copy, and of a row against zeros
   (tagpu_recorder.c: cap_units, rows_moved, row_emit). Which bytes it names decides what is imaged
   and so what the stream carries, so its answer is exact by definition, never approximate: the
   first index at or after `i` and before `n` where the bytes differ, or `n` when there is none, or
   `i` itself when `i` is not below `n`. tests/recdifftest.c holds it to that against the plain
   byte loop, on every length, start and alignment class it takes.

   Pure: no engine name and no Win32, so the test builds it on the host with the sanitizers as well
   as for Windows.

   SSE2, 64 bytes a step: the four 16-byte equality masks are ANDed and tested once; a step that
   differs is walked again 16 bytes at a time, where the first differing byte is the lowest clear bit
   of that block's mask; the last 0..15 bytes go one at a time. No load reaches past `n` and none
   before `i`, so a byte outside [i, n) is never read and never decides the answer. SSE2 is already
   code the DLL runs in play: the Makefile builds ImpAI's brain and the reclaim sweep's split with
   -msse2. The game's threads align the stack to 4 bytes only, and GCC's i686 target assumes 16 on entry to a function that may spill a vector (GCC 13's output, read
   2026-11-13: a spilled __m128i is reloaded as an aligned memory operand of pcmpeqb), so the
   function is built to realign its stack whenever it takes a vector slot there, rather than resting
   on the register allocator keeping every vector in a register, which GCC 13 does today. */

#include <emmintrin.h>

__attribute__((target("sse2"), force_align_arg_pointer))
static unsigned rec_diff_from(const unsigned char* a, const unsigned char* b, unsigned i, unsigned n)
{
    if (i >= n) return i;
    while (n - i >= 64) {
        __m128i e0 = _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)(a + i)),
                                    _mm_loadu_si128((const __m128i*)(b + i)));
        __m128i e1 = _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)(a + i + 16)),
                                    _mm_loadu_si128((const __m128i*)(b + i + 16)));
        __m128i e2 = _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)(a + i + 32)),
                                    _mm_loadu_si128((const __m128i*)(b + i + 32)));
        __m128i e3 = _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)(a + i + 48)),
                                    _mm_loadu_si128((const __m128i*)(b + i + 48)));
        if (_mm_movemask_epi8(_mm_and_si128(_mm_and_si128(e0, e1), _mm_and_si128(e2, e3))) != 0xFFFF)
            break;
        i += 64;
    }
    while (n - i >= 16) {
        unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(
                         _mm_loadu_si128((const __m128i*)(a + i)),
                         _mm_loadu_si128((const __m128i*)(b + i)))) ^ 0xFFFFu;
        if (m) return i + (unsigned)__builtin_ctz(m);
        i += 16;
    }
    while (i < n && a[i] == b[i]) i++;
    return i;
}

#endif
