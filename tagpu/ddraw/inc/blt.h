#ifndef BLT_H
#define BLT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>


extern BOOL g_blt_use_avx;

void blt_copy(
    unsigned char* dst,
    unsigned char* src,
    size_t size);

void blt_clean(
    unsigned char* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned char* src,
    int src_x,
    int src_y,
    int src_p,
    int bpp);
    
void blt_overlap(
    unsigned char* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned char* src,
    int src_x,
    int src_y,
    int src_p,
    int bpp);

void blt_colorkey(
    unsigned char* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned char* src,
    int src_x,
    int src_y,
    int src_p,
    unsigned int key_low,
    unsigned int key_high,
    int bpp);

void blt_colorkey_mirror_stretch(
    unsigned char* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned char* src,
    int src_x,
    int src_y,
    int src_w,
    int src_h,
    int src_p,
    unsigned int key_low,
    unsigned int key_high,
    BOOL mirror_up_down,
    BOOL mirror_left_right,
    int bpp);

void blt_clear(
    unsigned char* dst, 
    char color,
    size_t size);

void blt_colorfill(
    unsigned char* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned int color,
    int bpp);

void blt_rgb565_to_rgba8888(
    unsigned int* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned short* src,
    int src_x,
    int src_y,
    int src_p);

void blt_rgb555_to_rgba8888(
    unsigned int* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned short* src,
    int src_x,
    int src_y,
    int src_p);

void blt_bgra8888_to_rgba8888(
    unsigned int* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned int* src,
    int src_x,
    int src_y,
    int src_p);

void blt_stretch(
    unsigned char* dst,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h,
    int dst_p,
    unsigned char* src,
    int src_x,
    int src_y,
    int src_w,
    int src_h,
    int src_p,
    int bpp);

#endif
