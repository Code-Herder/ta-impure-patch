#ifndef IDIRECTD3D_H 
#define IDIRECTD3D_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "d3dcaps.h"


DEFINE_GUID(IID_IDirect3D, 0x3BBA0080, 0x2421, 0x11CF, 0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56);
DEFINE_GUID(IID_IDirect3D2, 0x6aae1ec1, 0x662a, 0x11d0, 0x88, 0x9d, 0x00, 0xaa, 0x00, 0xbb, 0xb7, 0x6a);
DEFINE_GUID(IID_IDirect3D3, 0xbb223240, 0xe72b, 0x11d0, 0xa9, 0xb4, 0x00, 0xaa, 0x00, 0xc0, 0x99, 0x3e);
DEFINE_GUID(IID_IDirect3D7, 0xf5049e77, 0x4861, 0x11d2, 0xa4, 0x7, 0x0, 0xa0, 0xc9, 0x6, 0x29, 0xa8);

DEFINE_GUID(IID_IDirect3DNullDevice, 0x8767df22, 0xbacc, 0x11d1, 0x89, 0x69, 0x0, 0xa0, 0xc9, 0x6, 0x29, 0xa8);


#define DECLARE_D3D_INTERFACE(iface)    typedef struct iface { \
                                        struct iface##Vtbl FAR* lpVtbl; \
                                        ULONG ref; \
                                    } iface; \
                                    typedef struct iface##Vtbl iface##Vtbl; \
                                    struct iface##Vtbl

/* IID_IDirect3D */

DECLARE_D3D_INTERFACE(IDirect3DImpl)
{
    HRESULT(__stdcall * QueryInterface) (IDirect3DImpl*, const IID* const riid, LPVOID * ppvObj);
    ULONG(__stdcall * AddRef) (IDirect3DImpl*);
    ULONG(__stdcall * Release) (IDirect3DImpl*);

    HRESULT(__stdcall * Initialize)(IDirect3DImpl*, int);
    HRESULT(__stdcall * EnumDevices)(IDirect3DImpl*, LPD3DENUMDEVICESCALLBACK, LPVOID);
    HRESULT(__stdcall * CreateLight)(IDirect3DImpl*, int, int);
    HRESULT(__stdcall * CreateMaterial)(IDirect3DImpl*, int, int);
    HRESULT(__stdcall * CreateViewport)(IDirect3DImpl*, int, int);
    HRESULT(__stdcall * FindDevice)(IDirect3DImpl*, int, int);
};

extern struct IDirect3DImplVtbl g_d3d_vtbl;

/* IID_IDirect3D2 */

DECLARE_D3D_INTERFACE(IDirect3D2Impl)
{
    HRESULT(__stdcall * QueryInterface) (IDirect3D2Impl*, const IID* const riid, LPVOID * ppvObj);
    ULONG(__stdcall * AddRef) (IDirect3D2Impl*);
    ULONG(__stdcall * Release) (IDirect3D2Impl*);

    HRESULT(__stdcall * EnumDevices)(IDirect3D2Impl*, LPD3DENUMDEVICESCALLBACK, LPVOID);
    HRESULT(__stdcall * CreateLight)(IDirect3D2Impl*, int, int);
    HRESULT(__stdcall * CreateMaterial)(IDirect3D2Impl*, int, int);
    HRESULT(__stdcall * CreateViewport)(IDirect3D2Impl*, int, int);
    HRESULT(__stdcall * FindDevice)(IDirect3D2Impl*, int, int);
    HRESULT(__stdcall * CreateDevice)(IDirect3D2Impl*, int, int, int);
};

extern struct IDirect3D2ImplVtbl g_d3d2_vtbl;

/* IID_IDirect3D3 */

DECLARE_D3D_INTERFACE(IDirect3D3Impl)
{
    HRESULT(__stdcall * QueryInterface) (IDirect3D3Impl*, const IID* const riid, LPVOID * ppvObj);
    ULONG(__stdcall * AddRef) (IDirect3D3Impl*);
    ULONG(__stdcall * Release) (IDirect3D3Impl*);

    HRESULT(__stdcall * EnumDevices)(IDirect3D3Impl*, LPD3DENUMDEVICESCALLBACK, LPVOID);
    HRESULT(__stdcall * CreateLight)(IDirect3D3Impl*, int, int);
    HRESULT(__stdcall * CreateMaterial)(IDirect3D3Impl*, int, int);
    HRESULT(__stdcall * CreateViewport)(IDirect3D3Impl*, int, int);
    HRESULT(__stdcall * FindDevice)(IDirect3D3Impl*, int, int);
    HRESULT(__stdcall * CreateDevice)(IDirect3D3Impl*, int, int, int, int);
    HRESULT(__stdcall * CreateVertexBuffer)(IDirect3D3Impl*, int, int, int, int);
    HRESULT(__stdcall * EnumZBufferFormats)(IDirect3D3Impl*, int, int, int);
    HRESULT(__stdcall * EvictManagedTextures)(IDirect3D3Impl*);
};

extern struct IDirect3D3ImplVtbl g_d3d3_vtbl;

/* IID_IDirect3D7 */

DECLARE_D3D_INTERFACE(IDirect3D7Impl)
{
    HRESULT(__stdcall * QueryInterface) (IDirect3D7Impl*, const IID* const riid, LPVOID * ppvObj);
    ULONG(__stdcall * AddRef) (IDirect3D7Impl*);
    ULONG(__stdcall * Release) (IDirect3D7Impl*);

    HRESULT(__stdcall * EnumDevices)(IDirect3D7Impl*, LPD3DENUMDEVICESCALLBACK7, LPVOID);
    HRESULT(__stdcall * CreateDevice)(IDirect3D7Impl*, int, int, int);
    HRESULT(__stdcall * CreateVertexBuffer)(IDirect3D7Impl*, int, int, int);
    HRESULT(__stdcall * EnumZBufferFormats)(IDirect3D7Impl*, int, int, int);
    HRESULT(__stdcall * EvictManagedTextures)(IDirect3D7Impl*);
};

extern struct IDirect3D7ImplVtbl g_d3d7_vtbl;

#endif
