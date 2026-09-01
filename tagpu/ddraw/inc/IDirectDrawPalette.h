#ifndef IDIRECTDRAWPALETTE_H
#define IDIRECTDRAWPALETTE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ddraw.h"


struct IDirectDrawPaletteImpl;
struct IDirectDrawPaletteImplVtbl;

typedef struct IDirectDrawPaletteImpl
{
    struct IDirectDrawPaletteImplVtbl* lpVtbl;

    ULONG ref;

    int data_bgr[256];
    RGBQUAD data_rgb[256];
    DWORD flags;

} IDirectDrawPaletteImpl;

typedef struct IDirectDrawPaletteImplVtbl IDirectDrawPaletteImplVtbl;

#undef INTERFACE
#define INTERFACE IDirectDrawPaletteImpl
struct IDirectDrawPaletteImplVtbl
{
    /*** IUnknown methods ***/
    STDMETHOD(QueryInterface) (THIS_ REFIID riid, LPVOID FAR* ppvObj) PURE;
    STDMETHOD_(ULONG, AddRef) (THIS)  PURE;
    STDMETHOD_(ULONG, Release) (THIS) PURE;
    /*** IDirectDrawPalette methods ***/
    STDMETHOD(GetCaps)(THIS_ LPDWORD) PURE;
    STDMETHOD(GetEntries)(THIS_ DWORD, DWORD, DWORD, LPPALETTEENTRY) PURE;
    STDMETHOD(Initialize)(THIS_ LPDIRECTDRAW, DWORD, LPPALETTEENTRY) PURE;
    STDMETHOD(SetEntries)(THIS_ DWORD, DWORD, DWORD, LPPALETTEENTRY) PURE;
};

extern struct IDirectDrawPaletteImplVtbl g_ddp_vtbl;

#endif
