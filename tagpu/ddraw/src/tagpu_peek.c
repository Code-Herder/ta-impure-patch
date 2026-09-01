/* tagpu_peek.c — on-demand memory reads. Contract and spec grammar: inc/tagpu_peek.h. */

#include <windows.h>
#include <stdio.h>
#include "tagpu_peek.h"

#define PEEK_TRIGGER "tagpu_peek.trigger"
#define PEEK_MAX     256          /* bytes per s<N>/x<N> read */

static void plog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* Readable means committed and not guard/no-access: the whole point of a peek is
   that a wrong address costs a log line, not the process. */
static BOOL readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    const char* c = (const char*)p;

    while (n)
    {
        size_t span;

        if (!VirtualQuery(c, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            return FALSE;

        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            return FALSE;

        span = (size_t)((const char*)mbi.BaseAddress + mbi.RegionSize - c);

        if (span >= n)
            return TRUE;

        n -= span;
        c += span;
    }

    return TRUE;
}

/* hex integer; advances *p, returns FALSE if there were no digits */
static BOOL parse_hex(const char** p, unsigned int* out)
{
    const char* s = *p;
    unsigned int v = 0;
    int digits = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;

    for (; ; s++)
    {
        int d;

        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;

        v = v * 16 + (unsigned int)d;
        digits++;
    }

    if (!digits)
        return FALSE;

    *p = s;
    *out = v;
    return TRUE;
}

static void eval(const char* spec)
{
    const char* p = spec;
    unsigned int addr = 0, off;
    BOOL deref = FALSE;
    char out[1024];
    char val[3 * PEEK_MAX + 8];

    while (*p == ' ' || *p == '\t')
        p++;

    if (*p == '*')
    {
        deref = TRUE;
        p++;
    }

    if (!parse_hex(&p, &addr))
    {
        _snprintf(out, sizeof out, "peek: %s = <bad spec>", spec);
        out[sizeof out - 1] = 0;
        plog(out);
        return;
    }

    if (deref)
    {
        if (!readable((const void*)addr, 4))
        {
            _snprintf(out, sizeof out, "peek: %s @0x%08X = <unreadable pointer>", spec, addr);
            out[sizeof out - 1] = 0;
            plog(out);
            return;
        }

        addr = *(const volatile unsigned int*)addr;
    }

    while (*p == '+')
    {
        p++;

        if (!parse_hex(&p, &off))
        {
            _snprintf(out, sizeof out, "peek: %s = <bad offset>", spec);
            out[sizeof out - 1] = 0;
            plog(out);
            return;
        }

        addr += off;
    }

    /* size suffix */
    {
        unsigned int n = 4;
        char kind = 'u';

        if (*p == ':')
        {
            p++;

            if (*p == 's' || *p == 'x')
            {
                kind = *p++;

                if (!parse_hex(&p, &n))
                    n = 16;
            }
            else if (!parse_hex(&p, &n))
            {
                n = 4;
            }
        }

        if (kind == 'u' && n != 1 && n != 2 && n != 4)
            kind = 'x';

        if (n < 1)
            n = 1;

        if (n > PEEK_MAX)
            n = PEEK_MAX;

        if (!readable((const void*)addr, n))
        {
            _snprintf(out, sizeof out, "peek: %s @0x%08X = <unreadable>", spec, addr);
            out[sizeof out - 1] = 0;
            plog(out);
            return;
        }

        if (kind == 'u')
        {
            unsigned int v = n == 1 ? *(const volatile unsigned char*)addr :
                             n == 2 ? *(const volatile unsigned short*)addr :
                                      *(const volatile unsigned int*)addr;

            _snprintf(val, sizeof val, "%u (0x%0*X)", v, (int)(n * 2), v);
        }
        else if (kind == 's')
        {
            const unsigned char* b = (const unsigned char*)addr;
            unsigned int i, k = 0;

            val[k++] = '"';

            for (i = 0; i < n && b[i]; i++)
                val[k++] = (b[i] >= 0x20 && b[i] < 0x7F) ? (char)b[i] : '.';

            val[k++] = '"';
            val[k] = 0;
        }
        else
        {
            const unsigned char* b = (const unsigned char*)addr;
            unsigned int i, k = 0;

            for (i = 0; i < n; i++)
                k += (unsigned int)_snprintf(val + k, sizeof val - k, i ? " %02X" : "%02X", b[i]);

            val[sizeof val - 1] = 0;
        }

        _snprintf(out, sizeof out, "peek: %s @0x%08X = %s", spec, addr, val);
        out[sizeof out - 1] = 0;
        plog(out);
    }
}

void tagpu_peek_frame(unsigned int frame_counter)
{
    char buf[4096];
    DWORD got = 0;
    HANDLE h;
    char* line;

    if (frame_counter % 15)
        return;

    if (GetFileAttributesA(PEEK_TRIGGER) == INVALID_FILE_ATTRIBUTES)
        return;

    h = CreateFileA(PEEK_TRIGGER, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);

    if (h == INVALID_HANDLE_VALUE)
        return;

    if (!ReadFile(h, buf, sizeof(buf) - 1, &got, NULL))
        got = 0;

    CloseHandle(h);
    DeleteFileA(PEEK_TRIGGER);
    buf[got] = 0;

    for (line = buf; *line; )
    {
        char* end = line;

        while (*end && *end != '\n' && *end != '\r')
            end++;

        while (*end == '\n' || *end == '\r')
            *end++ = 0;

        if (*line && *line != '#')
            eval(line);

        line = end;
    }
}
