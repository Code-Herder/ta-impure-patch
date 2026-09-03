/* tagpu_title — window title suffix from tagpu_title.txt. See inc/tagpu_title.h */
#include <windows.h>
#include <stdio.h>

#include "tagpu_title.h"

#define TITLE_FILE  "tagpu_title.txt"
/* Two fields live in the label (wt: and tacli:), so 95 usable chars is not much
 * slack. TITLE_MAX is then sized from the WORST case rather than the expected one:
 * the composed title must never truncate, because tacli records the string it
 * expects and then finds the window by it. */
#define LABEL_MAX   96                              /* 95 usable + NUL */
#define BASE_MAX    128                             /* = dd.h's g_ddraw.title[128] */
#define TITLE_MAX   (BASE_MAX + 3 + LABEL_MAX)      /* base + " - " + label */

static void tlog(const char* m)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", m); fclose(f); }
}

/* tagpu_title.txt -> one line of printable ASCII, or 0 when there is nothing
 * usable. Filtering is not politeness: the value goes straight into a window
 * title, where a trailing newline or a control byte is a mangled title with no
 * error reported anywhere. */
static int read_label(char* out, int cap)
{
    HANDLE h;
    char buf[256];
    DWORD n = 0;
    int i, k = 0;

    h = CreateFileA(TITLE_FILE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, buf, sizeof buf - 1, &n, 0)) n = 0;
    CloseHandle(h);
    if (!n) return 0;
    buf[n] = 0;

    for (i = 0; buf[i] && k < cap - 1; i++) {
        if (buf[i] == '\r' || buf[i] == '\n') break;        /* first line only */
        if ((unsigned char)buf[i] < 0x20 || (unsigned char)buf[i] > 0x7E) continue;
        if (buf[i] == ' ' && k == 0) continue;              /* no leading blanks */
        out[k++] = buf[i];
    }
    while (k > 0 && out[k - 1] == ' ') k--;                 /* nor trailing ones */
    out[k] = 0;
    return k > 0;
}

void tagpu_title_apply(HWND hwnd, const char* base)
{
    static HWND s_hwnd;
    static char s_base[BASE_MAX];
    char label[LABEL_MAX], want[TITLE_MAX], cur[TITLE_MAX];

    if (!hwnd || !IsWindow(hwnd) || !base || !*base) return;
    if (!read_label(label, sizeof label)) return;

    /* Keep the base OURSELVES, once per window. The caller re-reads it off the
     * live window (dd.c's GetWindowText into g_ddraw.title), inside a gate that
     * re-opens: dd_Release restores the old wndproc and zeroes g_ddraw while the
     * window is still alive, so a second DirectDraw creation on the same HWND
     * runs the block again — and the "pristine" title it would hand us the second
     * time is the one we set the first time. Composing from that stacks a second
     * suffix, and a title that matches neither of tacli's two search patterns is
     * an instance whose window it can no longer find at all. A new HWND is a new
     * window, which carries TA's own title again, so re-capture on change. */
    if (hwnd != s_hwnd || !s_base[0]) {
        s_hwnd = hwnd;
        lstrcpynA(s_base, base, sizeof s_base);
    }

    /* MSVCRT's _snprintf does not NUL-terminate on truncation. TITLE_MAX cannot
     * be overrun by the real inputs; refuse rather than set a truncated title if
     * that ever stops being true. */
    if (lstrlenA(s_base) + 3 + lstrlenA(label) >= (int)sizeof want) return;
    _snprintf(want, sizeof want, "%s - %s", s_base, label);
    want[sizeof want - 1] = 0;

    cur[0] = 0;
    GetWindowTextA(hwnd, cur, sizeof cur);
    if (!lstrcmpA(cur, want)) return;      /* already ours — and so no log spam */

    if (SetWindowTextA(hwnd, want)) {
        char m[TITLE_MAX + 16];
        _snprintf(m, sizeof m, "title: \"%s\"", want);
        m[sizeof m - 1] = 0;
        tlog(m);
    }
}
