#ifndef FPS_LIMITER_H
#define FPS_LIMITER_H

#include <windows.h>


typedef struct _D3DKMT_WAITFORVERTICALBLANKEVENT {
    UINT hAdapter;
    UINT hDevice;
    UINT VidPnSourceId;
} D3DKMT_WAITFORVERTICALBLANKEVENT;

typedef struct _D3DKMT_OPENADAPTERFROMHDC {
    HDC  hDc;
    UINT hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
} D3DKMT_OPENADAPTERFROMHDC;

typedef struct _D3DKMT_CLOSEADAPTER {
    UINT hAdapter;
} D3DKMT_CLOSEADAPTER;

typedef NTSTATUS(WINAPI* D3DKMTWAITFORVERTICALBLANKEVENTPROC)(const D3DKMT_WAITFORVERTICALBLANKEVENT* Arg1);
typedef NTSTATUS(WINAPI* D3DKMTOPENADAPTERFROMHDCPROC)(D3DKMT_OPENADAPTERFROMHDC* Arg1);
typedef NTSTATUS(WINAPI* D3DKMTCLOSEADAPTERPROC)(D3DKMT_CLOSEADAPTER* Arg1);

typedef struct FPSLIMITER
{
    DWORD tick_start;
    DWORD tick_end;
    DWORD tick_length;
    LONGLONG tick_length_ns;
    HANDLE htimer;
    LARGE_INTEGER due_time;
    D3DKMT_WAITFORVERTICALBLANKEVENT vblank_event;
    D3DKMT_OPENADAPTERFROMHDC adapter;
    D3DKMT_CLOSEADAPTER close_adapter;
    HMODULE gdi32_dll;
    D3DKMTWAITFORVERTICALBLANKEVENTPROC D3DKMTWaitForVerticalBlankEvent;
    D3DKMTOPENADAPTERFROMHDCPROC D3DKMTOpenAdapterFromHdc;
    D3DKMTCLOSEADAPTERPROC D3DKMTCloseAdapter;
    BOOL got_adapter;
    BOOL initialized;
    CRITICAL_SECTION cs;
    BOOL cs_initialized;
} FPSLIMITER;

extern FPSLIMITER g_fpsl;

void fpsl_init();
BOOL fpsl_wait_for_vblank();
void fpsl_frame_start();
void fpsl_frame_end();

/* tagpu: THE RENDER THREAD IS fpsl_init's ONE OWNER after start-up. It closes
   the D3DKMT adapter the render thread waits on, and writes the tick fields the
   render thread paces by, so any other thread only REQUESTS it -- the render
   thread runs it at its next fpsl_frame_start. */
void fpsl_request_init(void);

/* THE ONE CAP IS VSYNC'S. With the store's vsync on, a backstop just above
   the refresh rate of the monitor the window is on, or above an assumed 60 Hz
   when that rate cannot be read (fpsl_init says why); off, none.
   fpsl_frame_start asks both every frame. */

#endif
