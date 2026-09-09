#include <windows.h>
#include "debug.h"
#include "winapi_hooks.h"
#include "dd.h"
#include "hook.h"
#include "utils.h"
#include "config.h"


BOOL g_mouse_locked;
HHOOK g_mouse_hook;
HOOKPROC g_mouse_proc;

/* tagpu: on a desktop shared with a human, the game must never move or fence the
   real pointer — the snap-to-window on activation steals the cursor mid-task.
   Drop `tagpu_nowarp.on` next to the exe to suppress both (tacli does by default).
   Cached: this is a per-instance property, and mouse_lock runs on activation. */
BOOL tagpu_mouse_nowarp(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = GetFileAttributesA("tagpu_nowarp.on") != INVALID_FILE_ATTRIBUTES;
    return cached != 0;
}

/* THE ONE TRANSFORM from a client-area point to the engine's logical screen
   (tagpu, G17b). It was inline in wndproc.c's button cases and nowhere else,
   which made the pointer path untestable: `tacli`'s injected clicks deliver
   GAME-space coordinates straight to the engine (tagpu_shield.c
   `deliver_mouse`), so they never traverse this arithmetic at all, and the
   G17b gate's exit -- "clicks land on the right gadget" -- could not be
   measured by the tooling that exists. Sharing it is what makes the harness's
   device-space click the same code a player's click takes, rather than a copy
   of it that can drift.

   Returns 1 when the point was inside the letterboxed viewport. Outside it the
   engine has always been handed the centre of its own screen, and that is kept
   exactly: a click on a letterbox bar is not a click at the nearest edge.
   Design: research/notes/gui-renderer.md 13.1. */
/* THE TRUE DEVICE POINTER (tagpu, G17c). The engine only ever learns a point
   in its own logical grid, so at k = 3 its cursor can only sit on multiples of
   three device pixels; ours is drawn from the client-area point the same
   message carried, which is where the pointer actually is. Kept here because
   mouse_client_to_game is the one place that conversion happens, so recording
   it there cannot drift from what the engine was told.

   -1 means "no client point is known" -- the state after an injected click,
   which sets the engine's cursor with no pointer behind it, and before the
   first message of a session. The reader falls back to the engine's own
   position scaled up, which is what the engine's cursor does anyway. */
static volatile LONG s_cliX = -1, s_cliY = -1;

void mouse_note_client(int cx, int cy)
{
    InterlockedExchange(&s_cliY, cy);
    /* x LAST: the reader gates on x, so a torn pair can never be published */
    InterlockedExchange(&s_cliX, cx);
}

void mouse_forget_client(void)
{
    InterlockedExchange(&s_cliX, -1);
}

int mouse_last_client(int* cx, int* cy)
{
    LONG x = InterlockedExchangeAdd(&s_cliX, 0);
    LONG y = InterlockedExchangeAdd(&s_cliY, 0);
    if (x < 0) return 0;
    if (cx) *cx = (int)x;
    if (cy) *cy = (int)y;
    return 1;
}

int mouse_client_to_game(int cx, int cy, int* gx, int* gy)
{
    int x, y, inside;

    inside = !(cx > g_ddraw.render.viewport.x + g_ddraw.render.viewport.width ||
               cx < g_ddraw.render.viewport.x ||
               cy > g_ddraw.render.viewport.y + g_ddraw.render.viewport.height ||
               cy < g_ddraw.render.viewport.y);

    if (!inside)
    {
        x = g_ddraw.width / 2;
        y = g_ddraw.height / 2;
        /* the engine is being handed the centre of its screen, which is NOT
           where the pointer is: drawing our cursor there would be a lie, so
           the recorded point is dropped and the fallback takes over */
        mouse_forget_client();
    }
    else
    {
        x = (DWORD)((cx - g_ddraw.render.viewport.x) * g_ddraw.mouse.unscale_x);
        y = (DWORD)((cy - g_ddraw.render.viewport.y) * g_ddraw.mouse.unscale_y);
        mouse_note_client(cx, cy);
    }

    /* The clamp keeps the ORIGINAL unsigned comparison. `g_ddraw.width` is a
       DWORD and the inline version in wndproc promoted `x` to unsigned against
       it, so before the first dd_SetDisplayMode (width 0) the result was 0, not
       -1. An `(int)` cast here would hand back -1, which deliver_mouse re-reads
       as TAGPU_M_HERE and wndproc stores as 0xFFFFFFFF (both landing reviewers
       spotted the difference). Guarded rather than cast so the degenerate state
       cannot produce a negative. */
    if (g_ddraw.width  && x > (int)g_ddraw.width  - 1) x = (int)g_ddraw.width  - 1;
    if (g_ddraw.height && y > (int)g_ddraw.height - 1) y = (int)g_ddraw.height - 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    if (gx) *gx = x;
    if (gy) *gy = y;
    return inside;
}

void mouse_lock()
{
    if (g_config.devmode || g_ddraw.bnet_active || !g_ddraw.hwnd)
        return;

    if (g_hook_active && !g_mouse_locked && !util_is_minimized(g_ddraw.hwnd))
    {
        int game_count = InterlockedExchangeAdd((LONG*)&g_ddraw.show_cursor_count, 0);
        int cur_count = real_ShowCursor(TRUE) - 1;
        real_ShowCursor(FALSE);

        if (cur_count > game_count)
        {
            while (real_ShowCursor(FALSE) > game_count);
        }
        else if (cur_count < game_count)
        {
            while (real_ShowCursor(TRUE) < game_count);
        }

        real_SetCursor((HCURSOR)InterlockedExchangeAdd((LONG*)&g_ddraw.old_cursor, 0));

        RECT rc = { 0 };
        real_GetClientRect(g_ddraw.hwnd, &rc);
        real_MapWindowPoints(g_ddraw.hwnd, HWND_DESKTOP, (LPPOINT)&rc, 2);
        OffsetRect(&rc, g_ddraw.render.viewport.x, g_ddraw.render.viewport.y);

        int cur_x = InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.x, 0);
        int cur_y = InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.y, 0);

        if (!tagpu_mouse_nowarp())
        {
            real_SetCursorPos(
                g_config.adjmouse ? (int)(rc.left + (cur_x * g_ddraw.mouse.scale_x)) : rc.left + cur_x,
                g_config.adjmouse ? (int)(rc.top + (cur_y * g_ddraw.mouse.scale_y)) : rc.top + cur_y);

            CopyRect(&rc, &g_ddraw.mouse.rc);
            real_MapWindowPoints(g_ddraw.hwnd, HWND_DESKTOP, (LPPOINT)&rc, 2);
            real_ClipCursor(&rc);
        }

        g_mouse_locked = TRUE;
    }
}

void mouse_unlock()
{
    if (g_config.devmode || !g_hook_active || !g_ddraw.hwnd)
        return;

    if (g_mouse_locked)
    {
        g_mouse_locked = FALSE;

        real_ClipCursor(NULL);
        ReleaseCapture();

        RECT rc = { 0 };
        real_GetClientRect(g_ddraw.hwnd, &rc);
        real_MapWindowPoints(g_ddraw.hwnd, HWND_DESKTOP, (LPPOINT)&rc, 2);
        OffsetRect(&rc, g_ddraw.render.viewport.x, g_ddraw.render.viewport.y);

        int cur_x = InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.x, 0);
        int cur_y = InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.y, 0);

        if (!tagpu_mouse_nowarp())
        {
            real_SetCursorPos(
                (int)(rc.left + (cur_x * g_ddraw.mouse.scale_x)),
                (int)(rc.top + (cur_y * g_ddraw.mouse.scale_y)));
        }

        real_SetCursor(LoadCursor(NULL, IDC_ARROW));

        while (real_ShowCursor(TRUE) < 0);
    }
}

LRESULT CALLBACK mouse_hook_proc(int Code, WPARAM wParam, LPARAM lParam)
{
    if (!g_ddraw.ref)
        return g_mouse_proc(Code, wParam, lParam);

    if (Code < 0 || (!g_config.devmode && !g_mouse_locked))
        return CallNextHookEx(g_mouse_hook, Code, wParam, lParam);

    fake_GetCursorPos(&((MOUSEHOOKSTRUCT*)lParam)->pt);

    return g_mouse_proc(Code, wParam, lParam);
}
