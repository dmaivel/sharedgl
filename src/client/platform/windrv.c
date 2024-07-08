#ifdef _WIN32

#include <windows.h>
#include <client/glimpl.h>
#include <client/platform/gldrv.h>
#include <client/platform/windrv.h>
#include <client/platform/icd.h>

#include <stdbool.h>

static PIXELFORMATDESCRIPTOR pfd_table[64] = { 0 };
static int pfd_count = 0;

static struct WGLCALLBACKS callbacks;

static int max_width, max_height, real_width, real_height;

ICD_SET_MAX_DIMENSIONS_DEFINITION(max_width, max_height, real_width, real_height);
ICD_RESIZE_DEFINITION(real_width, real_height);

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

static HMODULE module_handle = 0;
static BOOL do_vflip = TRUE;

void windrv_set_module_address(HMODULE module)
{
    module_handle = module;
}

void windrv_set_vflip(BOOL flip)
{
    do_vflip = flip;
}

static const char *wgl_extensions = "WGL_ARB_create_context WGL_ARB_create_context_profile WGL_ARB_extensions_string WGL_ARB_pixel_format";

const char* wglGetExtensionsStringARB(HDC hdc)
{
    return wgl_extensions;
}

HGLRC wglCreateContextAttribsARB(HDC hDC, HGLRC hShareContext, const int *attribList)
{
    return (HGLRC)1;
}

BOOL wglGetPixelFormatAttribivARB(HDC hdc,
                                  int iPixelFormat,
                                  int iLayerPlane,
                                  UINT nAttributes,
                                  const int *piAttributes,
                                  int *piValues)
{
    return TRUE;
}

BOOL wglGetPixelFormatAttribfvARB(HDC hdc,
                                  int iPixelFormat,
                                  int iLayerPlane,
                                  UINT nAttributes,
                                  const int *piAttributes,
                                  FLOAT *pfValues)
{
    return TRUE;
}

BOOL wglChoosePixelFormatARB(HDC hdc,
                             const int *piAttribIList,
                             const FLOAT *pfAttribFList,
                             UINT nMaxFormats,
                             int *piFormats,
                             UINT *nNumFormats)
{
    *piFormats = 1;
    *nNumFormats = 1;
    return TRUE;
}

static void pfd_add(
    bool doublebuffer, bool gdi, unsigned int accum,
    int rbits, int gbits, int bbits, int abits,
    int rshift, int gshift, int bshift, int ashift,
    int depthbits, int stencilbits)
{
    pfd_table[pfd_count].nSize = sizeof(pfd_table[pfd_count]);
    pfd_table[pfd_count].nVersion = 1;
    pfd_table[pfd_count].dwFlags = PFD_SUPPORT_OPENGL | PFD_SUPPORT_COMPOSITION | PFD_DRAW_TO_WINDOW;

    if (doublebuffer)
        pfd_table[pfd_count].dwFlags |= PFD_DOUBLEBUFFER | PFD_SWAP_EXCHANGE;

    if (gdi)
        pfd_table[pfd_count].dwFlags |= PFD_SUPPORT_GDI;

    pfd_table[pfd_count].iPixelType = PFD_TYPE_RGBA;
    pfd_table[pfd_count].iLayerType = PFD_MAIN_PLANE;

    pfd_table[pfd_count].cColorBits = rbits + gbits + bbits + abits;
    pfd_table[pfd_count].cRedBits = rbits;
    pfd_table[pfd_count].cRedShift = rshift;
    pfd_table[pfd_count].cGreenBits = gbits;
    pfd_table[pfd_count].cGreenShift = gshift;
    pfd_table[pfd_count].cBlueBits = bbits;
    pfd_table[pfd_count].cBlueShift = bshift;
    pfd_table[pfd_count].cAlphaBits = abits;
    pfd_table[pfd_count].cAlphaShift = ashift;
    pfd_table[pfd_count].cAccumBits = 4*accum;
    pfd_table[pfd_count].cAccumRedBits = accum;
    pfd_table[pfd_count].cAccumGreenBits = accum;
    pfd_table[pfd_count].cAccumBlueBits = accum;
    pfd_table[pfd_count].cAccumAlphaBits = accum;
    pfd_table[pfd_count].cDepthBits = depthbits;
    pfd_table[pfd_count].cStencilBits = stencilbits;
    pfd_table[pfd_count].cAuxBuffers = 0;
    pfd_table[pfd_count].iLayerType = 0;
    pfd_table[pfd_count].bReserved = 0;
    pfd_table[pfd_count].dwLayerMask = 0;
    pfd_table[pfd_count].dwVisibleMask = 0;
    pfd_table[pfd_count].dwDamageMask = 0;

    pfd_count++;
}

/*
 * accounts for accum
 */
static void pfd_add2(
    bool doublebuffer, bool gdi, unsigned int accum,
    int rbits, int gbits, int bbits, int abits,
    int rshift, int gshift, int bshift, int ashift,
    int depthbits, int stencilbits)
{
    for (int i = 0; i < 2; i++)
        pfd_add(doublebuffer, gdi, i * accum, rbits, gbits, bbits, abits, rshift, gshift, bshift, ashift, depthbits, stencilbits);
}

static void pfd_init()
{
    /*
     * optional doublebuffer
     * no gdi
     */
    for (int i = 0; i < 2; i++) {
        for (int color = 0; color < 4; color++)
            for (int depth = 0; depth < 4; depth++)
                pfd_add2(i, false, 16, 
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
     * WGL extensions, GL functions
     * a table for WGL extensions makes sense, but we export every function so a table isn't needed
     * commented out || (lpszProc[0] == 'w' && lpszProc[1] == 'g' && lpszProc[2] == 'l'), some apps dont like that part
     */
    if ((lpszProc[0] == 'g' && lpszProc[1] == 'l'))
        return GetProcAddress(module_handle, lpszProc);

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

    static int init = 0;
    static void *framebuffer = NULL;

    if (!init) {
        bmi.bmiHeader.biWidth = max_width;
        bmi.bmiHeader.biHeight = -max_height;

        framebuffer = glimpl_fb_address();
        init = 1;
    }

    glimpl_swap_buffers(real_width, real_height, do_vflip, GL_BGRA); /* to-do: fix overlay so vflip and -Height won't be needed */
    SetDIBitsToDevice(hdc, 0, 0, real_width, real_height, 0, 0, 0, real_height, framebuffer, &bmi, DIB_RGB_COLORS);
    // StretchDIBits(hdc, 0, 0, real_width, real_height, 0, 0, real_width, real_height, framebuffer, &bmi, DIB_RGB_COLORS, SRCCOPY);

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
    if (!pfd_count)
        pfd_init();

    --iPixelFormat;

    if (iPixelFormat >= pfd_count || iPixelFormat < 0 || cjpfd != sizeof(PIXELFORMATDESCRIPTOR))
        return pfd_count;

    if (ppfd != NULL)
        memcpy(ppfd, &pfd_table[iPixelFormat], sizeof(PIXELFORMATDESCRIPTOR));
 
    return pfd_count;    
}

BOOL APIENTRY DrvSetPixelFormat(HDC hdc, LONG iPixelFormat)
{
    if (GetPixelFormat(hdc) == 0)
        SetPixelFormat(hdc, iPixelFormat, NULL);

    return TRUE;
}

#endif