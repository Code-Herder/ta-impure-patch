#include <windows.h>
#include <commctrl.h>
#include <limits.h>
#include "dd.h"
#include "tagpu_log.h"
#include "tagpu_vk.h"
#include "tagpu_window.h"

static const char s_property[] = "Impure.WindowLifetime";
static volatile LONG s_generation;

LONG tagpu_window_generation(HWND hwnd)
{
    return hwnd ? (LONG)(UINT_PTR)GetPropA(hwnd, s_property) : 0;
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR id, DWORD_PTR data)
{
    (void)data;
    if (msg == WM_DESTROY) {
        int owns_presentation;
        /* Publish the destruction before joining: a render thread exiting in
           this interval must tear down rather than retain its last image.
           The subclass survives DirectDraw's own window-procedure restoration. */
        owns_presentation = tagpu_vk_wndproc(hwnd, msg, wp, lp);
        RemovePropA(hwnd, s_property);
        if (g_ddraw.ref && g_ddraw.hwnd == hwnd) {
            dd_RestoreDisplayMode();
            if (owns_presentation) tagpu_vk_render_stop();
        } else if (!g_ddraw.ref && owns_presentation) {
            /* Final Release already joined the renderer before clearing this
               state. With no renderer, the window thread owns the retained lane. */
            tagpu_vk_render_stop();
        }
    }
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, window_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

int tagpu_window_attach(HWND hwnd)
{
    LONG prev, next;
    if (tagpu_window_generation(hwnd)) return 1;
    if (!hwnd || GetWindowThreadProcessId(hwnd, NULL) != GetCurrentThreadId()) {
        tagpu_log("window: attachment refused outside the window's owner thread");
        return 0;
    }
    do {
        prev = InterlockedCompareExchange(&s_generation, 0, 0);
        if (prev == LONG_MAX) {
            tagpu_log("window: lifetime IDs exhausted");
            return 0;
        }
        next = prev + 1;
    } while (InterlockedCompareExchange(&s_generation, next, prev) != prev);
    if (!SetPropA(hwnd, s_property, (HANDLE)(UINT_PTR)next)) return 0;
    if (!SetWindowSubclass(hwnd, window_proc, 0, 0)) {
        RemovePropA(hwnd, s_property);
        return 0;
    }
    tagpu_logf("window: attached lifetime %ld to %p", next, hwnd);
    return 1;
}
