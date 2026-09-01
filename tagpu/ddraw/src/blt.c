#include <windows.h>
#include <intrin.h>
#include "debug.h"
#include "blt.h"


BOOL g_blt_use_avx;

void blt_copy(
    unsigned char* dst,
    unsigned char* src,
    size_t size)
{
#if defined(_MSC_VER) || defined(__AVX__)
    if (!((DWORD)dst % 64) && !((DWORD)src % 64))
    {
        if (size >= 1024 * 4096 && g_blt_use_avx)
        {
            _mm_prefetch((const char*)(src), _MM_HINT_NTA);

            while (size >= 256)
            {
                __m256i c0 = _mm256_load_si256(((const __m256i*)src) + 0);
                __m256i c1 = _mm256_load_si256(((const __m256i*)src) + 1);
                __m256i c2 = _mm256_load_si256(((const __m256i*)src) + 2);
                __m256i c3 = _mm256_load_si256(((const __m256i*)src) + 3);
                __m256i c4 = _mm256_load_si256(((const __m256i*)src) + 4);
                __m256i c5 = _mm256_load_si256(((const __m256i*)src) + 5);
                __m256i c6 = _mm256_load_si256(((const __m256i*)src) + 6);
                __m256i c7 = _mm256_load_si256(((const __m256i*)src) + 7);

                _mm_prefetch((const char*)(src + 512), _MM_HINT_NTA);

                _mm256_stream_si256((((__m256i*)dst) + 0), c0);
                _mm256_stream_si256((((__m256i*)dst) + 1), c1);
                _mm256_stream_si256((((__m256i*)dst) + 2), c2);
                _mm256_stream_si256((((__m256i*)dst) + 3), c3);
                _mm256_stream_si256((((__m256i*)dst) + 4), c4);
                _mm256_stream_si256((((__m256i*)dst) + 5), c5);
                _mm256_stream_si256((((__m256i*)dst) + 6), c6);
                _mm256_stream_si256((((__m256i*)dst) + 7), c7);

                src += 256;
                dst += 256;
                size -= 256;
            }

            _mm_sfence();
            _mm256_zeroupper();
        }
        else if (size < 1024 * 100 && g_blt_use_avx)
        {
            while (size >= 128)
            {
                __m256i c0 = _mm256_load_si256(((const __m256i*)src) + 0);
                __m256i c1 = _mm256_load_si256(((const __m256i*)src) + 1);
                __m256i c2 = _mm256_load_si256(((const __m256i*)src) + 2);
                __m256i c3 = _mm256_load_si256(((const __m256i*)src) + 3);

                _mm256_store_si256((((__m256i*)dst) + 0), c0);
                _mm256_store_si256((((__m256i*)dst) + 1), c1);
                _mm256_store_si256((((__m256i*)dst) + 2), c2);
                _mm256_store_si256((((__m256i*)dst) + 3), c3);

                src += 128;
                dst += 128;
                size -= 128;
            }

            _mm256_zeroupper();
        }
        else if (size >= 1024 * 100)
        {
            __movsb(dst, src, size);

            size = 0;
        }

        /* memcpy below handles the remainder */
    }
#endif
    
    if (size > 0)
    {
        memcpy(dst, src, size);
    }
}

void blt_clean(
    unsigned char* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned char* src,
    int src_x,
    int src_y,
    int src_p,
    int bpp)
{
    int bytes_pp = bpp / 8;

    size_t size = dst_w * bytes_pp;

    src += (src_x * bytes_pp) + (src_p * src_y);
    dst += (dst_x * bytes_pp) + (dst_p * dst_y);

    if (size == dst_p && dst_p == src_p)
    {
        blt_copy(dst, src, dst_p * dst_h);
    }
    else
    {
        for (int i = 0; i < dst_h; i++)
        {
            blt_copy(dst, src, size);

            src += src_p;
            dst += dst_p;
        }
    }
}

void blt_overlap(
    unsigned char* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned char* src,
    int src_x,
    int src_y,
    int src_p,
    int bpp)
{
    int bytes_pp = bpp / 8;

    size_t size = dst_w * bytes_pp;

    src += (src_x * bytes_pp) + (src_p * src_y);
    dst += (dst_x * bytes_pp) + (dst_p * dst_y);

    if (dst_y > src_y)
    {
        src += src_p * dst_h;
        dst += dst_p * dst_h;

        for (int i = dst_h; i-- > 0;)
        {
            src -= src_p;
            dst -= dst_p;

            memmove(dst, src, size);
        }
    }
    else if (size == dst_p && dst_p == src_p)
    {
        memmove(dst, src, dst_p * dst_h);
    }
    else
    {
        for (int i = 0; i < dst_h; i++)
        {
            memmove(dst, src, size);

            src += src_p;
            dst += dst_p;
        }
    }
}

void blt_colorkey(
    unsigned char* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned char* src,
    int src_x,
    int src_y,
    int src_p,
    unsigned int key_low,
    unsigned int key_high,
    int bpp)
{
    int bytes_pp = bpp / 8;

    size_t s_a = (src_p / bytes_pp) - dst_w;
    size_t d_a = (dst_p / bytes_pp) - dst_w;

    src += (src_x * bytes_pp) + (src_p * src_y);
    dst += (dst_x * bytes_pp) + (dst_p * dst_y);

    if (bpp == 8)
    {
        unsigned char key_l = (unsigned char)key_low;
        unsigned char key_h = (unsigned char)key_high;

        if (key_l == key_h)
        {
            for (unsigned char* h_end = dst + dst_h * (dst_w + d_a); dst < h_end;)
            {
                for (unsigned char* w_end = dst + dst_w; dst < w_end;)
                {
                    unsigned char c = *src++;

                    if (c != key_l)
                    {
                        *dst = c;
                    }

                    dst++;
                }
                
                src += s_a;
                dst += d_a;
            }
        }
        else
        {
            for (unsigned char* h_end = dst + dst_h * (dst_w + d_a); dst < h_end;)
            {
                for (unsigned char* w_end = dst + dst_w; dst < w_end;)
                {
                    unsigned char c = *src++;

                    if (c < key_l || c > key_h)
                    {
                        *dst = c;
                    }

                    dst++;
                }

                src += s_a;
                dst += d_a;
            }
        }
    }
    else if (bpp == 16)
    {
        unsigned short key_l = (unsigned short)key_low;
        unsigned short key_h = (unsigned short)key_high;

        unsigned short* d = (unsigned short*)dst;
        unsigned short* s = (unsigned short*)src;

        if (key_l == key_h)
        {
            for (unsigned short* h_end = d + dst_h * (dst_w + d_a); d < h_end;)
            {
                for (unsigned short* w_end = d + dst_w; d < w_end;)
                {
                    unsigned short c = *s++;

                    if (c != key_l)
                    {
                        *d = c;
                    }

                    d++;
                }

                s += s_a;
                d += d_a;
            }
        }
        else
        {
            for (unsigned short* h_end = d + dst_h * (dst_w + d_a); d < h_end;)
            {
                for (unsigned short* w_end = d + dst_w; d < w_end;)
                {
                    unsigned short c = *s++;

                    if (c < key_l || c > key_h)
                    {
                        *d = c;
                    }

                    d++;
                }

                s += s_a;
                d += d_a;
            }
        }
    }
    else if (bpp == 32)
    {
        unsigned int key_l = key_low & 0xFFFFFF;
        unsigned int key_h = key_high & 0xFFFFFF;

        unsigned int* d = (unsigned int*)dst;
        unsigned int* s = (unsigned int*)src;

        if (key_l == key_h)
        {
            for (unsigned int* h_end = d + dst_h * (dst_w + d_a); d < h_end;)
            {
                for (unsigned int* w_end = d + dst_w; d < w_end;)
                {
                    unsigned int c = *s++;

                    if ((c & 0xFFFFFF) != key_l)
                    {
                        *d = c;
                    }

                    d++;
                }

                s += s_a;
                d += d_a;
            }
        }
        else
        {
            for (unsigned int* h_end = d + dst_h * (dst_w + d_a); d < h_end;)
            {
                for (unsigned int* w_end = d + dst_w; d < w_end;)
                {
                    unsigned int c = *s++;

                    if ((c & 0xFFFFFF) < key_l || (c & 0xFFFFFF) > key_h)
                    {
                        *d = c;
                    }

                    d++;
                }

                s += s_a;
                d += d_a;
            }
        }
    }
}

void blt_colorkey_mirror_stretch(
    unsigned char* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned char* src,
    int src_x,
    int src_y,
    int src_w,
    int src_h,
    int src_p,
    unsigned int key_low,
    unsigned int key_high,
    BOOL mirror_up_down,
    BOOL mirror_left_right,
    int bpp)
{
    int bytes_pp = bpp / 8;

    int dst_surf_w = dst_p / bytes_pp;
    int src_surf_w = src_p / bytes_pp;

    float scale_w = (float)src_w / dst_w;
    float scale_h = (float)src_h / dst_h;

    if (bpp == 8)
    {
        unsigned char key_l = (unsigned char)key_low;
        unsigned char key_h = (unsigned char)key_high;

        for (int y = 0; y < dst_h; y++)
        {
            int scaled_y = (int)(y * scale_h);

            if (mirror_up_down)
                scaled_y = src_h - 1 - scaled_y;

            int src_row = src_x + src_surf_w * (scaled_y + src_y);
            int dst_row = dst_x + dst_surf_w * (y + dst_y);

            for (int x = 0; x < dst_w; x++)
            {
                int scaled_x = (int)(x * scale_w);

                if (mirror_left_right)
                    scaled_x = src_w - 1 - scaled_x;

                unsigned char c = ((unsigned char*)src)[scaled_x + src_row];

                if (c < key_l || c > key_h)
                {
                    ((unsigned char*)dst)[x + dst_row] = c;
                }
            }
        }
    }
    else if (bpp == 16)
    {
        unsigned short key_l = (unsigned short)key_low;
        unsigned short key_h = (unsigned short)key_high;

        for (int y = 0; y < dst_h; y++)
        {
            int scaled_y = (int)(y * scale_h);

            if (mirror_up_down)
                scaled_y = src_h - 1 - scaled_y;

            int src_row = src_x + src_surf_w * (scaled_y + src_y);
            int dst_row = dst_x + dst_surf_w * (y + dst_y);

            for (int x = 0; x < dst_w; x++)
            {
                int scaled_x = (int)(x * scale_w);

                if (mirror_left_right)
                    scaled_x = src_w - 1 - scaled_x;

                unsigned short c = ((unsigned short*)src)[scaled_x + src_row];

                if (c < key_l || c > key_h)
                {
                    ((unsigned short*)dst)[x + dst_row] = c;
                }
            }
        }
    }
    else if (bpp == 32)
    {
        unsigned int key_l = key_low & 0xFFFFFF;
        unsigned int key_h = key_high & 0xFFFFFF;

        for (int y = 0; y < dst_h; y++)
        {
            int scaled_y = (int)(y * scale_h);

            if (mirror_up_down)
                scaled_y = src_h - 1 - scaled_y;

            int src_row = src_x + src_surf_w * (scaled_y + src_y);
            int dst_row = dst_x + dst_surf_w * (y + dst_y);

            for (int x = 0; x < dst_w; x++)
            {
                int scaled_x = (int)(x * scale_w);

                if (mirror_left_right)
                    scaled_x = src_w - 1 - scaled_x;

                unsigned int c = ((unsigned int*)src)[scaled_x + src_row];

                if ((c & 0xFFFFFF) < key_l || (c & 0xFFFFFF) > key_h)
                {
                    ((unsigned int*)dst)[x + dst_row] = c;
                }
            }
        }
    }
}

void blt_clear(
    unsigned char* dst,
    char color,
    size_t size)
{
#if defined(_MSC_VER) || defined(__AVX__)
    if (size < 1024 * 100 && g_blt_use_avx && !((DWORD)dst % 32))
    {
        __m256i c0 = _mm256_set1_epi8(color);

        while (size >= 128)
        {
            _mm256_store_si256((((__m256i*)dst) + 0), c0);
            _mm256_store_si256((((__m256i*)dst) + 1), c0);
            _mm256_store_si256((((__m256i*)dst) + 2), c0);
            _mm256_store_si256((((__m256i*)dst) + 3), c0);

            dst += 128;
            size -= 128;
        }

        _mm256_zeroupper();

        /* memset below handles the remainder */
    }
#endif

    if (size >= 1024 * 100)
    {
        __stosb(dst, color, size);
    }
    else
    {
        memset(dst, color, size);
    }
}

void blt_colorfill(
    unsigned char* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned int color,
    int bpp)
{
    int bytes_pp = bpp / 8;

    size_t size = dst_w * bytes_pp;

    dst += (dst_x * bytes_pp) + (dst_p * dst_y);

    if (bpp == 8 ||
        (bpp == 16 && 
            (color & 0xFF) == ((color >> 8) & 0xFF)) ||
        ((bpp == 32 || bpp == 24) &&
            (color & 0xFF) == ((color >> 8) & 0xFF) &&
            (color & 0xFF) == ((color >> 16) & 0xFF) &&
            (color & 0xFF) == ((color >> 24) & 0xFF)))
    {
        if (size == dst_p)
        {
            blt_clear(dst, color, dst_p * dst_h);
        }
        else
        {
            for (int i = 0; i < dst_h; i++)
            {
                blt_clear(dst, color, size);
                dst += dst_p;
            }
        }
    }
    else if (bpp == 16)
    {
        unsigned short* first_row = (unsigned short*)dst;

        for (int x = 0; x < dst_w; x++)
        {
            first_row[x] = (unsigned short)color;
        }

        for (int i = 1; i < dst_h; i++)
        {
            dst += dst_p;
            blt_copy(dst, (void*)first_row, size);
        }
    }
    else if (bpp == 32)
    {
        unsigned int* first_row = (unsigned int*)dst;

        for (int x = 0; x < dst_w; x++)
        {
            first_row[x] = color;
        }

        for (int i = 1; i < dst_h; i++)
        {
            dst += dst_p;
            blt_copy(dst, (void*)first_row, size);
        }
    }
}

void blt_rgb565_to_rgba8888(
    unsigned int* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned short* src,
    int src_x,
    int src_y,
    int src_p)
{
    size_t s_a = (src_p / sizeof(src[0])) - dst_w;
    size_t d_a = (dst_p / sizeof(dst[0])) - dst_w;

    src += src_x + ((src_p / sizeof(src[0])) * src_y);
    dst += dst_x + ((dst_p / sizeof(dst[0])) * dst_y);

    for (unsigned int* h_end = dst + dst_h * (dst_w + d_a); dst < h_end;)
    {
        for (unsigned int* w_end = dst + dst_w; dst < w_end;)
        {
            unsigned short pixel = *src++;

            BYTE r = ((pixel & 0xF800) >> 11) << 3;
            BYTE g = ((pixel & 0x07E0) >> 5) << 2;
            BYTE b = ((pixel & 0x001F)) << 3;

            *dst++ = (0xFF << 24) | (b << 16) | (g << 8) | r;
        }

        src += s_a;
        dst += d_a;
    }
}

void blt_rgb555_to_rgba8888(
    unsigned int* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned short* src,
    int src_x,
    int src_y,
    int src_p)
{
    size_t s_a = (src_p / sizeof(src[0])) - dst_w;
    size_t d_a = (dst_p / sizeof(dst[0])) - dst_w;

    src += src_x + ((src_p / sizeof(src[0])) * src_y);
    dst += dst_x + ((dst_p / sizeof(dst[0])) * dst_y);

    for (unsigned int* h_end = dst + dst_h * (dst_w + d_a); dst < h_end;)
    {
        for (unsigned int* w_end = dst + dst_w; dst < w_end;)
        {
            unsigned short pixel = *src++;

            BYTE r = ((pixel & 0x7C00) >> 10) << 3;
            BYTE g = ((pixel & 0x03E0) >> 5) << 3;
            BYTE b = ((pixel & 0x001F)) << 3;

            *dst++ = (0xFF << 24) | (b << 16) | (g << 8) | r;
        }

        src += s_a;
        dst += d_a;
    }
}

void blt_bgra8888_to_rgba8888(
    unsigned int* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned int* src,
    int src_x,
    int src_y,
    int src_p)
{
    size_t s_a = (src_p / sizeof(src[0])) - dst_w;
    size_t d_a = (dst_p / sizeof(dst[0])) - dst_w;

    src += src_x + ((src_p / sizeof(src[0])) * src_y);
    dst += dst_x + ((dst_p / sizeof(dst[0])) * dst_y);

    for (unsigned int* h_end = dst + dst_h * (dst_w + d_a); dst < h_end;)
    {
        for (unsigned int* w_end = dst + dst_w; dst < w_end;)
        {
            unsigned int pixel = *src++;

            BYTE r = pixel >> 16;
            BYTE g = pixel >> 8;
            BYTE b = pixel;

            *dst++ = (0xFF << 24) | (b << 16) | (g << 8) | r;
        }

        src += s_a;
        dst += d_a;
    }
}

void blt_stretch(
    unsigned char* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned char* src,
    int src_x,
    int src_y,
    int src_w,
    int src_h,
    int src_p,
    int bpp)
{
    int bytes_pp = bpp / 8;

    size_t size = dst_w * bytes_pp;

    int dst_surf_w = dst_p / bytes_pp;
    int src_surf_w = src_p / bytes_pp;

    float scale_w = (float)src_w / dst_w;
    float scale_h = (float)src_h / dst_h;

    int last_y = -1;
    int last_row = -1;

    if (bpp == 8)
    {
        unsigned char* d = (unsigned char*)dst;
        unsigned char* s = (unsigned char*)src;

        for (int y = 0; y < dst_h; y++)
        {
            int scaled_y = (int)(y * scale_h);
            int dst_row = dst_x + dst_surf_w * (y + dst_y);

            if (scaled_y == last_y)
            {
                blt_copy(&d[dst_row], &d[last_row], size);
                continue;
            }

            last_y = scaled_y;
            last_row = dst_row;

            int src_row = src_x + src_surf_w * (scaled_y + src_y);

            for (int x = 0; x < dst_w; x++)
            {
                int scaled_x = (int)(x * scale_w);

                d[x + dst_row] = s[scaled_x + src_row];
            }
        }
    }
    else if (bpp == 16)
    {
        unsigned short* d = (unsigned short*)dst;
        unsigned short* s = (unsigned short*)src;

        for (int y = 0; y < dst_h; y++)
        {
            int scaled_y = (int)(y * scale_h);
            int dst_row = dst_x + dst_surf_w * (y + dst_y);

            if (scaled_y == last_y)
            {
                blt_copy((void*)&d[dst_row], (void*)&d[last_row], size);
                continue;
            }

            last_y = scaled_y;
            last_row = dst_row;

            int src_row = src_x + src_surf_w * (scaled_y + src_y);

            for (int x = 0; x < dst_w; x++)
            {
                int scaled_x = (int)(x * scale_w);

                d[x + dst_row] = s[scaled_x + src_row];
            }
        }
    }
    else if (bpp == 32)
    {
        unsigned int* d = (unsigned int*)dst;
        unsigned int* s = (unsigned int*)src;

        for (int y = 0; y < dst_h; y++)
        {
            int scaled_y = (int)(y * scale_h);
            int dst_row = dst_x + dst_surf_w * (y + dst_y);

            if (scaled_y == last_y)
            {
                blt_copy((void*)&d[dst_row], (void*)&d[last_row], size);
                continue;
            }

            last_y = scaled_y;
            last_row = dst_row;

            int src_row = src_x + src_surf_w * (scaled_y + src_y);

            for (int x = 0; x < dst_w; x++)
            {
                int scaled_x = (int)(x * scale_w);

                d[x + dst_row] = s[scaled_x + src_row];
            }
        }
    }
}
