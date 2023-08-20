#ifdef _WIN32

#include <windows.h>
#include <client/glimpl.h>
#include <client/platform/gldrv.h>

#include <stdbool.h>

static HWND capturedWindow;
static HDC capturedHdc;
static int capturedWidth;
static int capturedHeight;

static PIXELFORMATDESCRIPTOR pfdTable[64] = { 0 };
static int pfdCount = 0;

static struct WGLCALLBACKS callbacks;

#define MIN_INTERNAL( A, B )   ( (A)<(B) ? (A) : (B) )
#define MAX_INTERNAL( A, B )   ( (A)>(B) ? (A) : (B) )

struct pfd_color_info {
    int rbits, gbits, bbits, abits;
    int rshift, gshift, bshift, ashift;
};

struct pfd_depth_info {
    int depthbits, stencilbits;
};

static struct pfd_color_info pfd_colors[4] = {
    { 8, 8, 8, 0, 16, 8, 0, 0 },
    { 8, 8, 8, 0, 8, 16, 24, 0 },
    { 8, 8, 8, 8, 16, 8, 0, 24 },
    { 8, 8, 8, 8, 8, 16, 24, 0 }
};

static struct pfd_depth_info pfd_depths[4] = {
    { 32, 0 },
    { 24, 0 },
    { 16, 0 },
    { 24, 8 }
};

static void pfdAdd(
    bool doublebuffer, bool gdi, unsigned int accum,
    int rbits, int gbits, int bbits, int abits,
    int rshift, int gshift, int bshift, int ashift,
    int depthbits, int stencilbits)
{
    pfdTable[pfdCount].nSize = sizeof(pfdTable[pfdCount]);
    pfdTable[pfdCount].nVersion = 1;
    pfdTable[pfdCount].dwFlags = PFD_SUPPORT_OPENGL | PFD_SUPPORT_COMPOSITION | PFD_DRAW_TO_WINDOW;

    if (doublebuffer)
        pfdTable[pfdCount].dwFlags |= PFD_DOUBLEBUFFER | PFD_SWAP_EXCHANGE;

    if (gdi)
        pfdTable[pfdCount].dwFlags |= PFD_SUPPORT_GDI;

    pfdTable[pfdCount].iPixelType = PFD_TYPE_RGBA;
    pfdTable[pfdCount].iLayerType = PFD_MAIN_PLANE;

    pfdTable[pfdCount].cColorBits = rbits + gbits + bbits + abits;
    pfdTable[pfdCount].cRedBits = rbits;
    pfdTable[pfdCount].cRedShift = rshift;
    pfdTable[pfdCount].cGreenBits = gbits;
    pfdTable[pfdCount].cGreenShift = gshift;
    pfdTable[pfdCount].cBlueBits = bbits;
    pfdTable[pfdCount].cBlueShift = bshift;
    pfdTable[pfdCount].cAlphaBits = abits;
    pfdTable[pfdCount].cAlphaShift = ashift;
    pfdTable[pfdCount].cAccumBits = 4*accum;
    pfdTable[pfdCount].cAccumRedBits = accum;
    pfdTable[pfdCount].cAccumGreenBits = accum;
    pfdTable[pfdCount].cAccumBlueBits = accum;
    pfdTable[pfdCount].cAccumAlphaBits = accum;
    pfdTable[pfdCount].cDepthBits = depthbits;
    pfdTable[pfdCount].cStencilBits = stencilbits;
    pfdTable[pfdCount].cAuxBuffers = 0;
    pfdTable[pfdCount].iLayerType = 0;
    pfdTable[pfdCount].bReserved = 0;
    pfdTable[pfdCount].dwLayerMask = 0;
    pfdTable[pfdCount].dwVisibleMask = 0;
    pfdTable[pfdCount].dwDamageMask = 0;

    pfdCount++;
}

/*
 * accounts for accum
 */
static void pfdAdd2(
    bool doublebuffer, bool gdi, unsigned int accum,
    int rbits, int gbits, int bbits, int abits,
    int rshift, int gshift, int bshift, int ashift,
    int depthbits, int stencilbits)
{
    for (int i = 0; i < 2; i++)
        pfdAdd(doublebuffer, gdi, i * accum, rbits, gbits, bbits, abits, rshift, gshift, bshift, ashift, depthbits, stencilbits);
}

static void pfdInit()
{
    /*
     * optional doublebuffer
     * no gdi
     */
    for (int i = 0; i < 2; i++) {
        for (int color = 0; color < 4; color++)
            for (int depth = 0; depth < 4; depth++)
                pfdAdd2(i, false, 16, 
                    pfd_colors[color].rbits, pfd_colors[color].gbits, pfd_colors[color].bbits, pfd_colors[color].abits,
                    pfd_colors[color].rshift, pfd_colors[color].gshift, pfd_colors[color].bshift, pfd_colors[color].ashift, 
                    pfd_depths[depth].depthbits, pfd_depths[depth].stencilbits);
    }
}

DHGLRC APIENTRY DrvCreateContext(HDC hdc)
{
    return (DHGLRC)1;
}

DHGLRC APIENTRY DrvCreateLayerContext(HDC hdc, INT iLayerPlane)
{
    return (DHGLRC)1;
}

BOOL APIENTRY DrvDeleteContext(DHGLRC dhglrc)
{
    return TRUE;
}

BOOL APIENTRY DrvCopyContext(DHGLRC dhrcSource, DHGLRC dhrcDest, UINT fuMask)
{
    return TRUE;
}

PGLCLTPROCTABLE APIENTRY DrvSetContext(HDC hdc, DHGLRC dhglrc, PFN_SETPROCTABLE pfnSetProcTable)
{
    capturedHdc = hdc;
    capturedWindow = WindowFromDC(capturedHdc);

    RECT Rect;
    if (GetClientRect(capturedWindow, &Rect)) {
        capturedWidth = Rect.right - Rect.left;
        capturedHeight = Rect.bottom - Rect.top;
    }

    return glimpl_GetProcTable();
}

BOOL APIENTRY DrvReleaseContext(DHGLRC dhglrc)
{
    return TRUE;
}

BOOL APIENTRY DrvValidateVersion(ULONG ulVersion)
{
    return TRUE;
}

BOOL APIENTRY DrvShareLists(DHGLRC dhglrc1, DHGLRC dhglrc2)
{
    return TRUE;
}

PROC APIENTRY DrvGetProcAddress(LPCSTR lpszProc)
{
    /*
     * WGL Extensions
     */
    if (lpszProc[0] == 'w' && lpszProc[1] == 'g' && lpszProc[2] == 'l')
        return NULL;

    if (lpszProc[0] == 'g' && lpszProc[1] == 'l')
        return GetProcAddress(GetModuleHandleW(NULL), lpszProc);

    return NULL;
}

void APIENTRY DrvSetCallbackProcs(INT nProcs, PROC *pProcs)
{
    size_t size = MIN_INTERNAL(nProcs * sizeof(*pProcs), sizeof(callbacks));
    memcpy(&callbacks, pProcs, size);

    return;
}

BOOL APIENTRY DrvDescribeLayerPlane(HDC hdc, INT iPixelFormat, INT iLayerPlane, UINT nBytes, LPLAYERPLANEDESCRIPTOR plpd)
{
    return FALSE;
}

int APIENTRY DrvGetLayerPaletteEntries(HDC hdc, INT iLayerPlane, INT iStart, INT cEntries, COLORREF *pcr) 
{
    return 0;
}

int APIENTRY DrvSetLayerPaletteEntries(HDC hdc, INT iLayerPlane, INT iStart, INT cEntries, CONST COLORREF *pcr)
{
    return 0;
}

BOOL APIENTRY DrvRealizeLayerPalette(HDC hdc, INT iLayerPlane, BOOL bRealize)
{
    return FALSE;
}

BOOL APIENTRY DrvSwapBuffers(HDC hdc)
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
        bmi.bmiHeader.biWidth = capturedWidth;
        bmi.bmiHeader.biHeight = -capturedHeight;

        glimpl_report(capturedWidth, capturedHeight);

        Frame = glimpl_fb_address();
        Init = 1;
    }

    glimpl_swap_buffers(capturedWidth, capturedHeight, 1, GL_BGRA); /* to-do: fix overlay so vflip and -Height won't be needed */
    SetDIBitsToDevice(capturedHdc, 0, 0, capturedWidth, capturedHeight, 0, 0, 0, capturedHeight, Frame, &bmi, DIB_RGB_COLORS);
    // StretchDIBits(Hdc, 0, 0, Width, Height, 0, 0, Width, Height, Frame, &bmi, DIB_RGB_COLORS, SRCCOPY);

    return TRUE;
}

BOOL APIENTRY DrvPresentBuffers(HDC hdc, LPPRESENTBUFFERS data)
{
    return TRUE;
}

BOOL APIENTRY DrvSwapLayerBuffers(HDC hdc, UINT fuPlanes)
{
    if (fuPlanes & WGL_SWAP_MAIN_PLANE)
        return DrvSwapBuffers(hdc);

    return FALSE;
}

LONG APIENTRY DrvDescribePixelFormat(HDC hdc, INT iPixelFormat, ULONG cjpfd, PIXELFORMATDESCRIPTOR *ppfd)
{
    if (!pfdCount)
        pfdInit();

    --iPixelFormat;

    if (iPixelFormat >= pfdCount || iPixelFormat < 0 || cjpfd != sizeof(PIXELFORMATDESCRIPTOR))
        return pfdCount;

    if (ppfd != NULL)
        memcpy(ppfd, &pfdTable[iPixelFormat], sizeof(PIXELFORMATDESCRIPTOR));
 
    return pfdCount;    
}

BOOL APIENTRY DrvSetPixelFormat(HDC hdc, LONG iPixelFormat)
{
    if (GetPixelFormat(hdc) == 0)
        SetPixelFormat(hdc, iPixelFormat, NULL);

    return TRUE;
}

#endif