#ifndef IDIRECTDRAWCLIPPER_H
#define IDIRECTDRAWCLIPPER_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ddraw.h"


struct IDirectDrawClipperImpl;
struct IDirectDrawClipperImplVtbl;

typedef struct IDirectDrawClipperImpl
{
    struct IDirectDrawClipperImplVtbl* lpVtbl;

    ULONG ref;
    HWND hwnd;
    HRGN region;
    CRITICAL_SECTION cs;

} IDirectDrawClipperImpl;

typedef struct IDirectDrawClipperImplVtbl IDirectDrawClipperImplVtbl;

#undef INTERFACE
#define INTERFACE IDirectDrawClipperImpl
struct IDirectDrawClipperImplVtbl
{
    /*** IUnknown methods ***/
    STDMETHOD(QueryInterface) (THIS_ REFIID riid, LPVOID FAR* ppvObj) PURE;
    STDMETHOD_(ULONG, AddRef) (THIS)  PURE;
    STDMETHOD_(ULONG, Release) (THIS) PURE;
    /*** IDirectDrawClipper methods ***/
    STDMETHOD(GetClipList)(THIS_ LPRECT, LPRGNDATA, LPDWORD) PURE;
    STDMETHOD(GetHWnd)(THIS_ HWND FAR*) PURE;
    STDMETHOD(Initialize)(THIS_ LPDIRECTDRAW, DWORD) PURE;
    STDMETHOD(IsClipListChanged)(THIS_ BOOL FAR*) PURE;
    STDMETHOD(SetClipList)(THIS_ LPRGNDATA, DWORD) PURE;
    STDMETHOD(SetHWnd)(THIS_ DWORD, HWND) PURE;
};

extern struct IDirectDrawClipperImplVtbl g_ddc_vtbl;

#endif
