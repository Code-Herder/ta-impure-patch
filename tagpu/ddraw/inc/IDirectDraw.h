#ifndef IDIRECTDRAW_H 
#define IDIRECTDRAW_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ddraw.h"


struct IDirectDrawImpl;
struct IDirectDrawImplVtbl;

typedef struct IDirectDrawImpl
{
    struct IDirectDrawImplVtbl* lpVtbl;

    ULONG ref;
    GUID guid;

} IDirectDrawImpl;

typedef struct IDirectDrawImplVtbl IDirectDrawImplVtbl;

#undef INTERFACE
#define INTERFACE IDirectDrawImpl
struct IDirectDrawImplVtbl
{
    /*** IUnknown methods ***/
    STDMETHOD(QueryInterface) (THIS_ REFIID riid, LPVOID FAR* ppvObj) PURE;
    STDMETHOD_(ULONG, AddRef) (THIS)  PURE;
    STDMETHOD_(ULONG, Release) (THIS) PURE;
    /*** IDirectDraw methods ***/
    STDMETHOD(Compact)(THIS) PURE;
    STDMETHOD(CreateClipper)(THIS_ DWORD, LPDIRECTDRAWCLIPPER FAR*, IUnknown FAR*) PURE;
    STDMETHOD(CreatePalette)(THIS_ DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE FAR*, IUnknown FAR*) PURE;
    STDMETHOD(CreateSurface)(THIS_  LPDDSURFACEDESC2, LPDIRECTDRAWSURFACE7 FAR*, IUnknown FAR*) PURE;
    STDMETHOD(DuplicateSurface)(THIS_ LPDIRECTDRAWSURFACE7, LPDIRECTDRAWSURFACE7 FAR*) PURE;
    STDMETHOD(EnumDisplayModes)(THIS_ DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMMODESCALLBACK2) PURE;
    STDMETHOD(EnumSurfaces)(THIS_ DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMSURFACESCALLBACK7) PURE;
    STDMETHOD(FlipToGDISurface)(THIS) PURE;
    STDMETHOD(GetCaps)(THIS_ LPDDCAPS, LPDDCAPS) PURE;
    STDMETHOD(GetDisplayMode)(THIS_ LPDDSURFACEDESC2) PURE;
    STDMETHOD(GetFourCCCodes)(THIS_  LPDWORD, LPDWORD) PURE;
    STDMETHOD(GetGDISurface)(THIS_ LPDIRECTDRAWSURFACE7 FAR*) PURE;
    STDMETHOD(GetMonitorFrequency)(THIS_ LPDWORD) PURE;
    STDMETHOD(GetScanLine)(THIS_ LPDWORD) PURE;
    STDMETHOD(GetVerticalBlankStatus)(THIS_ LPBOOL) PURE;
    STDMETHOD(Initialize)(THIS_ GUID FAR*) PURE;
    STDMETHOD(RestoreDisplayMode)(THIS) PURE;
    STDMETHOD(SetCooperativeLevel)(THIS_ HWND, DWORD) PURE;
    union
    {
        LPVOID d;
        STDMETHOD(SetDisplayMode)(THIS_ DWORD, DWORD, DWORD) PURE;
        STDMETHOD(SetDisplayModeX)(THIS_ DWORD, DWORD, DWORD, DWORD, DWORD) PURE;
    };
    STDMETHOD(WaitForVerticalBlank)(THIS_ DWORD, HANDLE) PURE;
    /*** Added in the v2 interface ***/
    STDMETHOD(GetAvailableVidMem)(THIS_ LPDDSCAPS2, LPDWORD, LPDWORD) PURE;
    /*** Added in the V4 Interface ***/
    STDMETHOD(GetSurfaceFromDC) (THIS_ HDC, LPDIRECTDRAWSURFACE7*) PURE;
    STDMETHOD(RestoreAllSurfaces)(THIS) PURE;
    STDMETHOD(TestCooperativeLevel)(THIS) PURE;
    STDMETHOD(GetDeviceIdentifier)(THIS_ LPDDDEVICEIDENTIFIER2, DWORD) PURE;
    /*** Added in the V7 Interface ***/
    STDMETHOD(StartModeTest)(THIS_ LPSIZE, DWORD, DWORD) PURE;
    STDMETHOD(EvaluateMode)(THIS_ DWORD, DWORD*) PURE;
};

extern struct IDirectDrawImplVtbl g_dd_vtbl1;
extern struct IDirectDrawImplVtbl g_dd_vtblx;

#endif
