#ifdef _WIN32

#include <client/platform/wgl.h>
#include <client/platform/minhook/MinHook.h>

#include <client/glimpl.h>

#include <windows.h>

#include <string.h>

HWND Window;
HDC Hdc;
int Width;
int Height;

BOOL(*SwapBuffersOriginal)(HDC unnamedParam1);
int(*ChoosePixelFormatOriginal)(HDC hdc, const PIXELFORMATDESCRIPTOR *ppfd);
BOOL(*SetPixelFormatOriginal)(HDC hdc, int format, const PIXELFORMATDESCRIPTOR *ppfd);

GLAPI PROC wglGetProcAddress(LPCSTR unnamedParam1)
{
    return GetProcAddress(GetModuleHandleW(NULL), unnamedParam1);
}

BOOL wglMakeCurrent(HDC unnamedParam1, HGLRC unnamedParam2)
{
    Hdc = unnamedParam1;
    Window = WindowFromDC(Hdc);

    RECT Rect;
    if (GetClientRect(Window, &Rect)) {
        Width = Rect.right - Rect.left;
        Height = Rect.bottom - Rect.top;
    }    

    return TRUE;
}

BOOL wglDeleteContext(HGLRC unnamedParam1)
{
    return TRUE; // STUB
}

HGLRC wglCreateContext(HDC unnamedParam1)
{
    return (HGLRC)1; // STUB
}

HDC wglGetCurrentDC()
{
    return Hdc;
}

GLAPI BOOL SwapBuffersHook(HDC unnamedParam1)
{
    static BITMAPINFO bmi = {
        .bmiHeader.biSize = sizeof(BITMAPINFO),
        .bmiHeader.biPlanes = 1,
        .bmiHeader.biBitCount = 32,
        .bmiHeader.biCompression = BI_RGB,
        .bmiHeader.biSizeImage = 0,
        .bmiHeader.biClrUsed = 0,
        .bmiHeader.biClrImportant = 0,
        .bmiHeader.biXPelsPerMeter = 0,
        .bmiHeader.biYPelsPerMeter = 0
    };

    static int Init = 0;
    static void *Frame = NULL;

    if (!Init) {
        bmi.bmiHeader.biWidth = Width;
        bmi.bmiHeader.biHeight = -Height;

        Frame = glimpl_fb_address();
        Init = 1;
    }

    glimpl_swap_buffers(Width, Height, 1, GL_BGRA); /* to-do: fix overlay so vflip and -Height won't be needed */
    SetDIBitsToDevice(Hdc, 0, 0, Width, Height, 0, 0, 0, Height, Frame, &bmi, DIB_RGB_COLORS);
    // StretchDIBits(Hdc, 0, 0, Width, Height, 0, 0, Width, Height, Frame, &bmi, DIB_RGB_COLORS, SRCCOPY);

    return TRUE;
}

int ChoosePixelFormatHook(HDC hdc, const PIXELFORMATDESCRIPTOR *ppfd)
{
    return TRUE; // STUB
}

BOOL SetPixelFormatHook(HDC hdc, int format, const PIXELFORMATDESCRIPTOR *ppfd)
{
    return TRUE; // STUB
}

void WglInit()
{
    MH_Initialize();

    MH_CreateHook(SwapBuffers, SwapBuffersHook, (PVOID*)(&SwapBuffersOriginal));
    MH_EnableHook(SwapBuffers);

    MH_CreateHook(ChoosePixelFormat, ChoosePixelFormatHook, (PVOID*)(&ChoosePixelFormatOriginal));
    MH_EnableHook(ChoosePixelFormat);

    MH_CreateHook(SetPixelFormat, SetPixelFormatHook, (PVOID*)(&SetPixelFormatOriginal));
    MH_EnableHook(SetPixelFormat);
}

#endif