#ifndef IDIRECTDRAWGAMMACONTROL_H
#define IDIRECTDRAWGAMMACONTROL_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ddraw.h"


struct IDirectDrawGammaControlImpl;
struct IDirectDrawGammaControlImplVtbl;

typedef struct IDirectDrawGammaControlImpl
{
    struct IDirectDrawGammaControlImplVtbl* lpVtbl;

    ULONG ref;

} IDirectDrawGammaControlImpl;

typedef struct IDirectDrawGammaControlImplVtbl IDirectDrawGammaControlImplVtbl;

#undef INTERFACE
#define INTERFACE IDirectDrawGammaControlImpl
struct IDirectDrawGammaControlImplVtbl
{
    /*** IUnknown methods ***/
    STDMETHOD(QueryInterface) (THIS_ REFIID riid, LPVOID FAR* ppvObj) PURE;
    STDMETHOD_(ULONG, AddRef) (THIS)  PURE;
    STDMETHOD_(ULONG, Release) (THIS) PURE;
    /*** IDirectDrawGammaControl methods ***/
    STDMETHOD(GetGammaRamp)(THIS_ DWORD, LPDDGAMMARAMP) PURE;
    STDMETHOD(SetGammaRamp)(THIS_ DWORD, LPDDGAMMARAMP) PURE;
};

extern struct IDirectDrawGammaControlImplVtbl g_ddgc_vtbl;

#endif
