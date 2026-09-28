/* Image decoding through GDI+'s flat C API (part of Windows, no extra
   library to ship). Only the handful of entry points needed are declared
   here -- gdiplus.h itself is C++ only. */
#include "image.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>

typedef struct { UINT32 GdiplusVersion; void *DebugEventCallback; BOOL SuppressBackgroundThread; BOOL SuppressExternalCodecs; } GpStartupInput;
typedef struct { INT X, Y, Width, Height; } GpRect;
typedef struct { UINT Width, Height; INT Stride; INT PixelFormat; void *Scan0; UINT_PTR Reserved; } GpBitmapData;

int WINAPI GdiplusStartup(ULONG_PTR *token, const GpStartupInput *input, void *output);
int WINAPI GdipCreateBitmapFromFile(const WCHAR *filename, void **bitmap);
int WINAPI GdipGetImageWidth(void *image, UINT *width);
int WINAPI GdipGetImageHeight(void *image, UINT *height);
int WINAPI GdipBitmapLockBits(void *bitmap, const GpRect *rect, UINT flags, INT format, GpBitmapData *data);
int WINAPI GdipBitmapUnlockBits(void *bitmap, GpBitmapData *data);
int WINAPI GdipDisposeImage(void *image);

#define GP_LOCK_READ 1
#define GP_FORMAT_32BPP_ARGB 0x0026200A

uint32_t *image_load(const char *path, int *w, int *h) {
    static ULONG_PTR token = 0;
    if (!token) {
        GpStartupInput in = { 1, NULL, FALSE, FALSE };
        if (GdiplusStartup(&token, &in, NULL) != 0) { token = 0; return NULL; }
    }
    WCHAR wpath[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH * 2)) return NULL;
    DWORD attr = GetFileAttributesW(wpath);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return NULL;
    void *bmp = NULL;
    if (GdipCreateBitmapFromFile(wpath, &bmp) != 0 || !bmp) return NULL;
    UINT bw = 0, bh = 0;
    GdipGetImageWidth(bmp, &bw);
    GdipGetImageHeight(bmp, &bh);
    uint32_t *out = NULL;
    GpRect r = { 0, 0, (INT)bw, (INT)bh };
    GpBitmapData d;
    if (bw && bh && GdipBitmapLockBits(bmp, &r, GP_LOCK_READ, GP_FORMAT_32BPP_ARGB, &d) == 0) {
        out = (uint32_t *)malloc((size_t)bw * bh * 4);
        if (out)
            for (UINT y = 0; y < bh; y++) memcpy(out + (size_t)y * bw, (const uint8_t *)d.Scan0 + (size_t)y * d.Stride, (size_t)bw * 4);
        GdipBitmapUnlockBits(bmp, &d);
    }
    GdipDisposeImage(bmp);
    if (out) { *w = (int)bw; *h = (int)bh; }
    return out;
}
