#include <windows.h>
#include "fps_limiter.h"
#include "dd.h"
#include "debug.h"
#include "hook.h"
#include "config.h"
#include "versionhelpers.h"
#include "utils.h"
#include "tagpu_log.h"
#include "tagpu_settings.h"


FPSLIMITER g_fpsl;

/* tagpu: fps_limiter.h, "THE RENDER THREAD IS fpsl_init's ONE OWNER" */
static volatile LONG s_reinit;
/* the monitor the backstop was derived for, NULL with vsync off: the render
   thread's alone, like everything fpsl_init writes */
static HMONITOR s_pacedMon;

void fpsl_request_init(void) { InterlockedExchange(&s_reinit, 1); }

/* tagpu: the monitor the backstop sits above -- the window's, with vsync on --
   or NULL. MonitorFromWindow is a lookup, not a mode query. */
static HMONITOR paced_monitor(void)
{
    return tagpu_settings_vsync() ? MonitorFromWindow(g_ddraw.hwnd, MONITOR_DEFAULTTONEAREST) : NULL;
}

void fpsl_init()
{
    int max_fps = 0;
    HMONITOR mon;

    /* tagpu: the request is cleared BEFORE anything is read, so one made while
       this runs re-arms it rather than being lost. */
    InterlockedExchange(&s_reinit, 0);
    mon = s_pacedMon = paced_monitor();

    g_fpsl.tick_length_ns = 0;
    g_fpsl.tick_length = 0;

    /* tagpu: THE ONE CAP IS VSYNC'S BACKSTOP, AT hz + 1, which a present that
       waits for the vertical blank can never meet. The timer only waits when a
       frame ended sooner after the last one than its period, so a period
       shorter than the blank's is only met by jitter, and that wait ends before
       the next blank. A timer AT the refresh rate is not that: Windows reports
       59 Hz for a 59.94 Hz mode, and a 59 fps timer against it skips a blank
       about once a second. `hz` is a whole number and the true rate lies within
       one of it whether the driver rounds down, up or to nearest, so hz + 1 is
       always the faster clock.
       The backstop is there for a present that does NOT wait: the Windows AMD
       test card's FIFO swapchain presented 300 fps at 59 Hz (MEASURED
       2026-09-25, in play), and the GDI backend has no present mode at all.
       Vsync off is no cap. A monitor whose rate cannot be read (0) leaves the
       present to pace alone, because no backstop can be placed above an
       unknown rate. */
    if (!mon)
    {
        tagpu_log("frame cap: none (vsync off)");
    }
    else
    {
        int hz = util_monitor_refresh(mon);

        if (hz > 0)
        {
            max_fps = hz + 1;
            tagpu_logf("frame cap: vsync on, a %d fps backstop over the monitor's %d Hz",
                       max_fps, hz);
        }
        else
        {
            tagpu_log("frame cap: vsync on and the monitor's rate cannot be read - "
                      "the present paces alone");
        }
    }

    if (max_fps > 0)
    {
        float len = 1000.0f / max_fps;
        g_fpsl.tick_length_ns = (LONGLONG)(len * 10000);
        g_fpsl.tick_length = (DWORD)len;// + 0.5f;
    }

    if (g_fpsl.got_adapter && g_fpsl.D3DKMTCloseAdapter)
    {
        g_fpsl.initialized = FALSE;
        g_fpsl.got_adapter = FALSE;
        g_fpsl.close_adapter.hAdapter = g_fpsl.adapter.hAdapter;
        g_fpsl.D3DKMTCloseAdapter(&g_fpsl.close_adapter);
    }

    if (!g_fpsl.cs_initialized)
    {
        g_fpsl.cs_initialized = TRUE;
        InitializeCriticalSection(&g_fpsl.cs);
    }

    if (!g_fpsl.gdi32_dll)
    {
        g_fpsl.gdi32_dll = real_LoadLibraryA("gdi32.dll");
    }

    if (!g_fpsl.D3DKMTWaitForVerticalBlankEvent)
    {
        g_fpsl.D3DKMTWaitForVerticalBlankEvent =
            (D3DKMTWAITFORVERTICALBLANKEVENTPROC)real_GetProcAddress(g_fpsl.gdi32_dll, "D3DKMTWaitForVerticalBlankEvent");
    }

    if (!g_fpsl.D3DKMTOpenAdapterFromHdc)
    {
        g_fpsl.D3DKMTOpenAdapterFromHdc =
            (D3DKMTOPENADAPTERFROMHDCPROC)real_GetProcAddress(g_fpsl.gdi32_dll, "D3DKMTOpenAdapterFromHdc");
    }

    if (!g_fpsl.D3DKMTCloseAdapter)
    {
        g_fpsl.D3DKMTCloseAdapter =
            (D3DKMTCLOSEADAPTERPROC)real_GetProcAddress(g_fpsl.gdi32_dll, "D3DKMTCloseAdapter");
    }

    g_fpsl.initialized = TRUE;
}

BOOL fpsl_wait_for_vblank()
{
    if (g_fpsl.initialized)
    {
        if (!g_fpsl.got_adapter && g_fpsl.D3DKMTOpenAdapterFromHdc && g_ddraw.render.hdc)
        {
            EnterCriticalSection(&g_fpsl.cs);

            if (!g_fpsl.got_adapter)
            {
                g_fpsl.adapter.hDc = g_ddraw.render.hdc;

                if (g_fpsl.D3DKMTOpenAdapterFromHdc(&g_fpsl.adapter) == 0)
                {
                    g_fpsl.vblank_event.hAdapter = g_fpsl.adapter.hAdapter;
                    g_fpsl.got_adapter = TRUE;
                }
            }

            LeaveCriticalSection(&g_fpsl.cs);
        }

        if (g_fpsl.got_adapter && g_fpsl.D3DKMTWaitForVerticalBlankEvent)
        {
            return g_fpsl.D3DKMTWaitForVerticalBlankEvent(&g_fpsl.vblank_event) == 0;
        }
    }

    return FALSE;
}

void fpsl_frame_start()
{
    /* tagpu: VSYNC AND THE WINDOW'S MONITOR ARE ASKED EVERY FRAME, on either
       backend, so the frame that sees a change -- the menu's toggle, the
       window dragged to another monitor -- is the one the backstop is
       re-derived for; the monitor's mode is read only then. */
    if (paced_monitor() != s_pacedMon)
        InterlockedExchange(&s_reinit, 1);

    if (s_reinit)
        fpsl_init();

    if (g_fpsl.tick_length > 0)
        g_fpsl.tick_start = timeGetTime();
}

void fpsl_frame_end()
{
    if (g_fpsl.tick_length > 0)
    {
        if (g_fpsl.htimer)
        {
            FILETIME ft = { 0 };
            GetSystemTimeAsFileTime(&ft);

            if (CompareFileTime((FILETIME*)&g_fpsl.due_time, &ft) == -1)
            {
                memcpy(&g_fpsl.due_time, &ft, sizeof(LARGE_INTEGER));
            }
            else
            {
                WaitForSingleObject(g_fpsl.htimer, g_fpsl.tick_length * 2);
            }

            g_fpsl.due_time.QuadPart += g_fpsl.tick_length_ns;
            SetWaitableTimer(g_fpsl.htimer, &g_fpsl.due_time, 0, NULL, NULL, FALSE);
        }
        else
        {
            g_fpsl.tick_end = timeGetTime();

            if (g_fpsl.tick_end - g_fpsl.tick_start < g_fpsl.tick_length)
            {
                Sleep(g_fpsl.tick_length - (g_fpsl.tick_end - g_fpsl.tick_start));
            }
        }
    }
}
