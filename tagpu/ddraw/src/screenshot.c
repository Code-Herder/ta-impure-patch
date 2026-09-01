#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <time.h>
#include "dd.h"
#include "ddpalette.h"
#include "ddsurface.h"
#include "lodepng.h"
#include "blt.h"
#include "config.h"

static BOOL ss_screenshot_bmp(char* filename, IDirectDrawSurfaceImpl* src);

static BOOL ss_screenshot_8bit(char* filename, IDirectDrawSurfaceImpl* src)
{
    LodePNGState state;

    lodepng_state_init(&state);

    for (int i = 0; i < 256; i++)
    {
        RGBQUAD* c = &src->palette->data_rgb[i];
        lodepng_palette_add(&state.info_png.color, c->rgbRed, c->rgbGreen, c->rgbBlue, 255);
        lodepng_palette_add(&state.info_raw, c->rgbRed, c->rgbGreen, c->rgbBlue, 255);
    }

    state.info_png.color.colortype = LCT_PALETTE;
    state.info_png.color.bitdepth = 8;
    state.info_raw.colortype = LCT_PALETTE;
    state.info_raw.bitdepth = 8;
    state.encoder.auto_convert = 0;

    unsigned char* dst_buf = NULL;
    size_t dst_buf_size = 0;

    unsigned int error = 
        lodepng_encode(
            &dst_buf, 
            &dst_buf_size, 
            dds_GetBuffer(src), 
            src->pitch / src->bytes_pp, /* can't specify pitch so we use bitmap real width */
            src->height,
            &state);

    if (!error && dst_buf)
        lodepng_save_file(dst_buf, dst_buf_size, filename);

    lodepng_state_cleanup(&state);

    if (dst_buf)
        free(dst_buf);

    return !error;
}

static BOOL ss_screenshot_16bit(char* filename, IDirectDrawSurfaceImpl* src)
{
    unsigned int error = TRUE;
    unsigned int* buf = malloc(src->width * src->height * 4);

    if (buf)
    {
        if (g_config.rgb555)
        {
            blt_rgb555_to_rgba8888(
                buf,
                0,
                0,
                src->width,
                src->height,
                src->width * 4,
                dds_GetBuffer(src),
                0,
                0,
                src->pitch);
        }
        else
        {
            blt_rgb565_to_rgba8888(
                buf,
                0,
                0,
                src->width,
                src->height,
                src->width * 4,
                dds_GetBuffer(src),
                0,
                0,
                src->pitch);
        }

        error = lodepng_encode32_file(filename, (unsigned char*)buf, src->width, src->height);

        free(buf);
    }

    return !error;
}

static BOOL ss_screenshot_32bit(char* filename, IDirectDrawSurfaceImpl* src)
{
    unsigned int error = TRUE;
    unsigned int* buf = malloc(src->width * src->height * 4);

    if (buf)
    {
        blt_bgra8888_to_rgba8888(
            buf,
            0,
            0,
            src->width,
            src->height,
            src->width * 4,
            dds_GetBuffer(src),
            0,
            0,
            src->pitch);

        error = lodepng_encode32_file(filename, (unsigned char*)buf, src->width, src->height);

        free(buf);
    }

    return !error;
}

BOOL ss_take_screenshot(IDirectDrawSurfaceImpl* src)
{
    if (!src || !dds_GetBuffer(src) || !src->width || !src->height)
        return FALSE;

    char title[128];
    char filename[128];
    char str_time[64];
    time_t t = time(NULL);

    strncpy(title, g_ddraw.title, sizeof(g_ddraw.title));

    for (int i = 0; i < strlen(title); i++)
    {
        if (title[i] == ' ')
        {
            title[i] = '_';
        }
        else
        {
            title[i] = tolower(title[i]);
        }
    }

    CreateDirectoryA(g_config.screenshot_dir, NULL);

    strftime(str_time, sizeof(str_time), "%Y-%m-%d_%H-%M-%S", localtime(&t));
    _snprintf(filename, sizeof(filename) - 1, "%s%s_%s.png", g_config.screenshot_dir, title, str_time);

    if (FILE_EXISTS(filename))
        return FALSE;

    if (src->bpp == 8 && src->palette)
    {
        if (!ss_screenshot_8bit(filename, src))
        {
            memcpy(&src->bmi->bmiColors[0], src->palette->data_rgb, 256 * sizeof(int));

            return ss_screenshot_bmp(filename, src);
        }

        return TRUE;
    }
    else if (src->bpp == 16)
    {
        if (!ss_screenshot_16bit(filename, src))
            return ss_screenshot_bmp(filename, src);

        return TRUE;
    }
    else if (src->bpp == 32)
    {
        if (!ss_screenshot_32bit(filename, src))
            return ss_screenshot_bmp(filename, src);

        return TRUE;
    }
    
    return FALSE;
}

static BOOL ss_screenshot_bmp(char* filename, IDirectDrawSurfaceImpl* src)
{
    BOOL result = TRUE;

    // make sure file extension is correct
    char* ext = filename + strlen(filename) - 4;

    if (_strcmpi(ext, ".png") == 0)
    {
        strncpy(ext, ".bmp", 5);
    }

    // Create the .BMP file.  
    HANDLE hf = CreateFile(
        filename,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        (HANDLE)NULL);

    if (hf == INVALID_HANDLE_VALUE)
        return FALSE;

    PBITMAPINFOHEADER pbih = (PBITMAPINFOHEADER)src->bmi;
    BITMAPFILEHEADER hdr;

    hdr.bfType = 0x4d42;        // 0x42 = "B" 0x4d = "M"  
    // Compute the size of the entire file.  
    hdr.bfSize = (DWORD)(sizeof(BITMAPFILEHEADER) + pbih->biSize + pbih->biClrUsed * sizeof(RGBQUAD) + pbih->biSizeImage);
    hdr.bfReserved1 = 0;
    hdr.bfReserved2 = 0;

    // Compute the offset to the array of color indices.  
    hdr.bfOffBits = (DWORD)sizeof(BITMAPFILEHEADER) + pbih->biSize + pbih->biClrUsed * sizeof(RGBQUAD);

    // Copy the BITMAPFILEHEADER into the .BMP file.  
    DWORD tmp;
    if (!WriteFile(hf, (LPVOID)&hdr, sizeof(BITMAPFILEHEADER), (LPDWORD)&tmp, NULL))
    {
        result = FALSE;
    }

    // Copy the BITMAPINFOHEADER and RGBQUAD array into the file.  
    if (!WriteFile(hf, (LPVOID)pbih, sizeof(BITMAPINFOHEADER) + pbih->biClrUsed * sizeof(RGBQUAD), (LPDWORD)&tmp, (NULL)))
    {
        result = FALSE;
    }

    // Copy the array of color indices into the .BMP file.  
    DWORD cb = pbih->biSizeImage;
    if (!WriteFile(hf, (LPSTR)dds_GetBuffer(src), (int)cb, (LPDWORD)&tmp, NULL))
    {
        result = FALSE;
    }     

    // Close the .BMP file.  
    if (!CloseHandle(hf))
    {
        result = FALSE;
    }

    return result;
}
