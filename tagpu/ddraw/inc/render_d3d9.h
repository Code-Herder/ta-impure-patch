#ifndef RENDER_D3D9_H
#define RENDER_D3D9_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#define D3D9_TEXTURE_COUNT 2

typedef struct CUSTOMVERTEX { float x, y, z, rhw, u, v; } CUSTOMVERTEX;

typedef struct D3D9RENDERER
{
    HMODULE hmodule;
    D3DPRESENT_PARAMETERS params;
    HWND hwnd;
    LPDIRECT3D9 instance;
    LPDIRECT3DDEVICE9 device;
    LPDIRECT3DVERTEXBUFFER9 vertex_buf;
    IDirect3DTexture9* surface_tex[D3D9_TEXTURE_COUNT];
    IDirect3DTexture9* palette_tex[D3D9_TEXTURE_COUNT];
    IDirect3DPixelShader9* pixel_shader;
    IDirect3DPixelShader9* pixel_shader_upscale;
    float scale_w;
    float scale_h;
    int tex_width;
    int tex_height;
} D3D9RENDERER;

#define MAX_D3D9ON12_QUEUES        2

typedef struct _D3D9ON12_ARGS
{
    BOOL Enable9On12;
    IUnknown* pD3D12Device;
    IUnknown* ppD3D12Queues[MAX_D3D9ON12_QUEUES];
    UINT NumQueues;
    UINT NodeMask;
} D3D9ON12_ARGS;

BOOL d3d9_is_available();
DWORD WINAPI d3d9_render_main(void);
BOOL d3d9_create();
BOOL d3d9_reset(BOOL windowed);
BOOL d3d9_release_resources();
BOOL d3d9_release();
BOOL d3d9_on_device_lost();


#endif
