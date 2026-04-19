#ifdef _WIN32

#include <sharedgl.h>

#include <windows.h>
#include <client/glimpl.h>
#include <client/platform/gldrv.h>
#include <client/platform/windrv.h>
#include <client/platform/icd.h>

#include <client/pb.h>
#include <client/spinlock.h>

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef WGL_NUMBER_PIXEL_FORMATS_ARB
#define WGL_NUMBER_PIXEL_FORMATS_ARB      0x2000
#define WGL_DRAW_TO_WINDOW_ARB            0x2001
#define WGL_DRAW_TO_BITMAP_ARB            0x2002
#define WGL_ACCELERATION_ARB              0x2003
#define WGL_NEED_PALETTE_ARB              0x2004
#define WGL_NEED_SYSTEM_PALETTE_ARB       0x2005
#define WGL_SWAP_LAYER_BUFFERS_ARB        0x2006
#define WGL_SWAP_METHOD_ARB               0x2007
#define WGL_NUMBER_OVERLAYS_ARB           0x2008
#define WGL_NUMBER_UNDERLAYS_ARB          0x2009
#define WGL_TRANSPARENT_ARB               0x200A
#define WGL_SHARE_DEPTH_ARB               0x200C
#define WGL_SHARE_STENCIL_ARB             0x200D
#define WGL_SHARE_ACCUM_ARB               0x200E
#define WGL_SUPPORT_GDI_ARB               0x200F
#define WGL_SUPPORT_OPENGL_ARB            0x2010
#define WGL_DOUBLE_BUFFER_ARB             0x2011
#define WGL_STEREO_ARB                    0x2012
#define WGL_PIXEL_TYPE_ARB                0x2013
#define WGL_COLOR_BITS_ARB                0x2014
#define WGL_RED_BITS_ARB                  0x2015
#define WGL_RED_SHIFT_ARB                 0x2016
#define WGL_GREEN_BITS_ARB                0x2017
#define WGL_GREEN_SHIFT_ARB               0x2018
#define WGL_BLUE_BITS_ARB                 0x2019
#define WGL_BLUE_SHIFT_ARB                0x201A
#define WGL_ALPHA_BITS_ARB                0x201B
#define WGL_ALPHA_SHIFT_ARB               0x201C
#define WGL_ACCUM_BITS_ARB                0x201D
#define WGL_ACCUM_RED_BITS_ARB            0x201E
#define WGL_ACCUM_GREEN_BITS_ARB          0x201F
#define WGL_ACCUM_BLUE_BITS_ARB           0x2020
#define WGL_ACCUM_ALPHA_BITS_ARB          0x2021
#define WGL_DEPTH_BITS_ARB                0x2022
#define WGL_STENCIL_BITS_ARB              0x2023
#define WGL_AUX_BUFFERS_ARB               0x2024
#define WGL_NO_ACCELERATION_ARB           0x2025
#define WGL_GENERIC_ACCELERATION_ARB      0x2026
#define WGL_FULL_ACCELERATION_ARB         0x2027
#define WGL_SWAP_EXCHANGE_ARB             0x2028
#define WGL_SWAP_COPY_ARB                 0x2029
#define WGL_SWAP_UNDEFINED_ARB            0x202A
#define WGL_TYPE_RGBA_ARB                 0x202B
#define WGL_DRAW_TO_PBUFFER_ARB           0x202D
#define WGL_TRANSPARENT_RED_VALUE_ARB     0x2037
#define WGL_TRANSPARENT_GREEN_VALUE_ARB   0x2038
#define WGL_TRANSPARENT_BLUE_VALUE_ARB    0x2039
#define WGL_TRANSPARENT_ALPHA_VALUE_ARB   0x203A
#define WGL_TRANSPARENT_INDEX_VALUE_ARB   0x203B
#define WGL_SAMPLE_BUFFERS_ARB            0x2041
#define WGL_SAMPLES_ARB                   0x2042
#define WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB  0x20A9
#endif

static PIXELFORMATDESCRIPTOR pfd_table[64] = { 0 };
static int pfd_count = 0;

static struct WGLCALLBACKS callbacks;

static int max_width, max_height, real_width, real_height;

static void *swap_sync_lock;

ICD_SET_MAX_DIMENSIONS_DEFINITION(max_width, max_height, real_width, real_height);
ICD_RESIZE_DEFINITION(real_width, real_height);

#define MIN_INTERNAL( A, B )   ( (A)<(B) ? (A) : (B) )
#define MAX_INTERNAL( A, B )   ( (A)>(B) ? (A) : (B) )
#define WINDRV_MAX_CONTEXTS 64

typedef HGLRC(WINAPI * 	wglCreateContext_t )(HDC);
typedef BOOL(WINAPI * 	wglDeleteContext_t )(HGLRC);

static wglCreateContext_t g_pfnwglCreateContext = NULL;
static wglDeleteContext_t g_pfnwglDeleteContext = NULL;

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

static void pfd_init(void);

struct wgl_extension_entry {
   const char *name;
   PROC proc;
};

struct windrv_context_state {
    BOOL in_use;
    DHGLRC handle;
    HDC hdc;
    INT layer_plane;
};

static HMODULE module_handle = 0;
static BOOL do_vflip = TRUE;
static LONG next_context_handle = 0;
static struct windrv_context_state windrv_contexts[WINDRV_MAX_CONTEXTS] = { 0 };

static struct windrv_context_state *windrv_lookup_context(DHGLRC handle)
{
    if (handle == 0)
        return NULL;

    for (int i = 0; i < WINDRV_MAX_CONTEXTS; i++) {
        if (windrv_contexts[i].in_use && windrv_contexts[i].handle == handle)
            return &windrv_contexts[i];
    }

    return NULL;
}

static struct windrv_context_state *windrv_alloc_context(HDC hdc, INT layer_plane)
{
    for (int i = 0; i < WINDRV_MAX_CONTEXTS; i++) {
        struct windrv_context_state *context = &windrv_contexts[i];

        if (!context->in_use) {
            LONG raw_handle = InterlockedIncrement(&next_context_handle);

            if (raw_handle <= 0)
                raw_handle = InterlockedIncrement(&next_context_handle);

            *context = (struct windrv_context_state){
                .in_use = TRUE,
                .handle = (DHGLRC)raw_handle,
                .hdc = hdc,
                .layer_plane = layer_plane
            };
            return context;
        }
    }

    return NULL;
}

static BOOL windrv_free_context(DHGLRC handle)
{
    struct windrv_context_state *context = windrv_lookup_context(handle);

    if (context == NULL)
        return FALSE;

    *context = (struct windrv_context_state){ 0 };
    return TRUE;
}

static const PIXELFORMATDESCRIPTOR *windrv_get_pfd(int iPixelFormat)
{
    if (!pfd_count)
        pfd_init();

    if (iPixelFormat < 1 || iPixelFormat > pfd_count)
        return NULL;

    return &pfd_table[iPixelFormat - 1];
}

static int windrv_get_wgl_acceleration(const PIXELFORMATDESCRIPTOR *pfd)
{
    if ((pfd->dwFlags & PFD_GENERIC_FORMAT) != 0) {
        if ((pfd->dwFlags & PFD_GENERIC_ACCELERATED) != 0)
            return WGL_GENERIC_ACCELERATION_ARB;

        return WGL_NO_ACCELERATION_ARB;
    }

    return WGL_FULL_ACCELERATION_ARB;
}

static int windrv_get_wgl_swap_method(const PIXELFORMATDESCRIPTOR *pfd)
{
    if ((pfd->dwFlags & PFD_DOUBLEBUFFER) == 0)
        return WGL_SWAP_UNDEFINED_ARB;

    if ((pfd->dwFlags & PFD_SWAP_COPY) != 0)
        return WGL_SWAP_COPY_ARB;

    if ((pfd->dwFlags & PFD_SWAP_EXCHANGE) != 0)
        return WGL_SWAP_EXCHANGE_ARB;

    return WGL_SWAP_UNDEFINED_ARB;
}

static BOOL windrv_query_pixel_format_attribute(int iPixelFormat,
                                                int iLayerPlane,
                                                int attribute,
                                                int *value)
{
    const PIXELFORMATDESCRIPTOR *pfd;

    if (value == NULL)
        return FALSE;

    if (!pfd_count)
        pfd_init();

    if (attribute == WGL_NUMBER_PIXEL_FORMATS_ARB) {
        *value = pfd_count;
        return TRUE;
    }

    if (iLayerPlane != 0)
        return FALSE;

    pfd = windrv_get_pfd(iPixelFormat);
    if (pfd == NULL)
        return FALSE;

    switch (attribute) {
    case WGL_DRAW_TO_WINDOW_ARB:
        *value = 1;
        return TRUE;
    case WGL_DRAW_TO_BITMAP_ARB:
        *value = 0;
        return TRUE;
    case WGL_ACCELERATION_ARB:
        *value = windrv_get_wgl_acceleration(pfd);
        return TRUE;
    case WGL_NEED_PALETTE_ARB:
    case WGL_NEED_SYSTEM_PALETTE_ARB:
    case WGL_SWAP_LAYER_BUFFERS_ARB:
    case WGL_NUMBER_OVERLAYS_ARB:
    case WGL_NUMBER_UNDERLAYS_ARB:
    case WGL_TRANSPARENT_ARB:
    case WGL_TRANSPARENT_RED_VALUE_ARB:
    case WGL_TRANSPARENT_GREEN_VALUE_ARB:
    case WGL_TRANSPARENT_BLUE_VALUE_ARB:
    case WGL_TRANSPARENT_ALPHA_VALUE_ARB:
    case WGL_TRANSPARENT_INDEX_VALUE_ARB:
    case WGL_SHARE_DEPTH_ARB:
    case WGL_SHARE_STENCIL_ARB:
    case WGL_SHARE_ACCUM_ARB:
    case WGL_DRAW_TO_PBUFFER_ARB:
    case WGL_SAMPLE_BUFFERS_ARB:
    case WGL_SAMPLES_ARB:
        *value = 0;
        return TRUE;
    case WGL_SWAP_METHOD_ARB:
        *value = windrv_get_wgl_swap_method(pfd);
        return TRUE;
    case WGL_SUPPORT_GDI_ARB:
        *value = ((pfd->dwFlags & PFD_SUPPORT_GDI) != 0);
        return TRUE;
    case WGL_SUPPORT_OPENGL_ARB:
        *value = ((pfd->dwFlags & PFD_SUPPORT_OPENGL) != 0);
        return TRUE;
    case WGL_DOUBLE_BUFFER_ARB:
        *value = ((pfd->dwFlags & PFD_DOUBLEBUFFER) != 0);
        return TRUE;
    case WGL_STEREO_ARB:
        *value = ((pfd->dwFlags & PFD_STEREO) != 0);
        return TRUE;
    case WGL_PIXEL_TYPE_ARB:
        *value = WGL_TYPE_RGBA_ARB;
        return TRUE;
    case WGL_COLOR_BITS_ARB:
        *value = pfd->cColorBits;
        return TRUE;
    case WGL_RED_BITS_ARB:
        *value = pfd->cRedBits;
        return TRUE;
    case WGL_RED_SHIFT_ARB:
        *value = pfd->cRedShift;
        return TRUE;
    case WGL_GREEN_BITS_ARB:
        *value = pfd->cGreenBits;
        return TRUE;
    case WGL_GREEN_SHIFT_ARB:
        *value = pfd->cGreenShift;
        return TRUE;
    case WGL_BLUE_BITS_ARB:
        *value = pfd->cBlueBits;
        return TRUE;
    case WGL_BLUE_SHIFT_ARB:
        *value = pfd->cBlueShift;
        return TRUE;
    case WGL_ALPHA_BITS_ARB:
        *value = pfd->cAlphaBits;
        return TRUE;
    case WGL_ALPHA_SHIFT_ARB:
        *value = pfd->cAlphaShift;
        return TRUE;
    case WGL_ACCUM_BITS_ARB:
        *value = pfd->cAccumBits;
        return TRUE;
    case WGL_ACCUM_RED_BITS_ARB:
        *value = pfd->cAccumRedBits;
        return TRUE;
    case WGL_ACCUM_GREEN_BITS_ARB:
        *value = pfd->cAccumGreenBits;
        return TRUE;
    case WGL_ACCUM_BLUE_BITS_ARB:
        *value = pfd->cAccumBlueBits;
        return TRUE;
    case WGL_ACCUM_ALPHA_BITS_ARB:
        *value = pfd->cAccumAlphaBits;
        return TRUE;
    case WGL_DEPTH_BITS_ARB:
        *value = pfd->cDepthBits;
        return TRUE;
    case WGL_STENCIL_BITS_ARB:
        *value = pfd->cStencilBits;
        return TRUE;
    case WGL_AUX_BUFFERS_ARB:
        *value = pfd->cAuxBuffers;
        return TRUE;
    case WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB:
        *value = 0;
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOL windrv_match_pixel_format_attribute(int iPixelFormat, int attribute, int requested)
{
    int actual;

    if (!windrv_query_pixel_format_attribute(iPixelFormat, 0, attribute, &actual))
        return FALSE;

    switch (attribute) {
    case WGL_ACCELERATION_ARB:
    case WGL_SWAP_METHOD_ARB:
    case WGL_PIXEL_TYPE_ARB:
    case WGL_DRAW_TO_WINDOW_ARB:
    case WGL_DRAW_TO_BITMAP_ARB:
    case WGL_SUPPORT_GDI_ARB:
    case WGL_SUPPORT_OPENGL_ARB:
    case WGL_DOUBLE_BUFFER_ARB:
    case WGL_STEREO_ARB:
    case WGL_TRANSPARENT_ARB:
    case WGL_NEED_PALETTE_ARB:
    case WGL_NEED_SYSTEM_PALETTE_ARB:
    case WGL_SWAP_LAYER_BUFFERS_ARB:
    case WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB:
        return actual == requested;
    case WGL_COLOR_BITS_ARB:
    case WGL_RED_BITS_ARB:
    case WGL_GREEN_BITS_ARB:
    case WGL_BLUE_BITS_ARB:
    case WGL_ALPHA_BITS_ARB:
    case WGL_ACCUM_BITS_ARB:
    case WGL_ACCUM_RED_BITS_ARB:
    case WGL_ACCUM_GREEN_BITS_ARB:
    case WGL_ACCUM_BLUE_BITS_ARB:
    case WGL_ACCUM_ALPHA_BITS_ARB:
    case WGL_DEPTH_BITS_ARB:
    case WGL_STENCIL_BITS_ARB:
    case WGL_AUX_BUFFERS_ARB:
    case WGL_NUMBER_OVERLAYS_ARB:
    case WGL_NUMBER_UNDERLAYS_ARB:
    case WGL_SAMPLES_ARB:
        return actual >= requested;
    case WGL_SAMPLE_BUFFERS_ARB:
        if (requested == 0)
            return TRUE;

        return actual == requested;
    default:
        return actual == requested;
    }
}

static BOOL windrv_pixel_format_matches(int iPixelFormat, const int *attrib_list)
{
    if (attrib_list == NULL)
        return TRUE;

    while (*attrib_list != 0) {
        int attribute = attrib_list[0];
        int requested = attrib_list[1];

        if (!windrv_match_pixel_format_attribute(iPixelFormat, attribute, requested))
            return FALSE;

        attrib_list += 2;
    }

    return TRUE;
}

void windrv_set_module_address(HMODULE module)
{
    module_handle = module;
}

void windrv_set_vflip(BOOL flip)
{
    do_vflip = flip;
}

static const char *wgl_extensions =
    "WGL_ARB_create_context "
    "WGL_ARB_create_context_profile "
    "WGL_ARB_extensions_string "
    "WGL_ARB_framebuffer_sRGB "
    "WGL_ARB_pixel_format";

static HGLRC wgl_create_context(HDC hdc)
{
    if (g_pfnwglCreateContext == NULL)
        g_pfnwglCreateContext = (wglCreateContext_t)GetProcAddress(GetModuleHandleA("opengl32.dll"), "wglCreateContext");

    return g_pfnwglCreateContext(hdc);
}

static BOOL wgl_delete_context(HGLRC hglrc)
{
    if (g_pfnwglDeleteContext == NULL)
        g_pfnwglDeleteContext = (wglDeleteContext_t)GetProcAddress(GetModuleHandleA("opengl32.dll"), "wglDeleteContext");

    return g_pfnwglDeleteContext(hglrc);
}

const char* wglGetExtensionsStringARB(HDC hdc)
{
    (void)hdc;
    return wgl_extensions;
}

HGLRC wglCreateContextAttribsARB(HDC hDC, HGLRC hShareContext, const int *attribList)
{
    (void)hShareContext;
    (void)attribList;
    return wgl_create_context(hDC);
}

BOOL wglGetPixelFormatAttribivARB(HDC hdc,
                                  int iPixelFormat,
                                  int iLayerPlane,
                                  UINT nAttributes,
                                  const int *piAttributes,
                                  int *piValues)
{
    if (piAttributes == NULL || piValues == NULL)
        return FALSE;

    for (UINT i = 0; i < nAttributes; i++) {
        if (!windrv_query_pixel_format_attribute(iPixelFormat,
                                                 iLayerPlane,
                                                 piAttributes[i],
                                                 &piValues[i]))
            return FALSE;
    }

    (void)hdc;
    return TRUE;
}

BOOL wglGetPixelFormatAttribfvARB(HDC hdc,
                                  int iPixelFormat,
                                  int iLayerPlane,
                                  UINT nAttributes,
                                  const int *piAttributes,
                                  FLOAT *pfValues)
{
    int values[32];

    if (pfValues == NULL || piAttributes == NULL)
        return FALSE;

    if (nAttributes > (sizeof(values) / sizeof(values[0])))
        return FALSE;

    if (!wglGetPixelFormatAttribivARB(hdc, iPixelFormat, iLayerPlane, nAttributes, piAttributes, values))
        return FALSE;

    for (UINT i = 0; i < nAttributes; i++)
        pfValues[i] = (FLOAT)values[i];

    return TRUE;
}

BOOL wglChoosePixelFormatARB(HDC hdc,
                             const int *piAttribIList,
                             const FLOAT *pfAttribFList,
                             UINT nMaxFormats,
                             int *piFormats,
                             UINT *nNumFormats)
{
    UINT match_count = 0;

    if (nNumFormats == NULL)
        return FALSE;

    if (pfAttribFList != NULL && pfAttribFList[0] != 0.0f) {
        *nNumFormats = 0;
        return TRUE;
    }

    if (!pfd_count)
        pfd_init();

    for (int i = 1; i <= pfd_count; i++) {
        if (!windrv_pixel_format_matches(i, piAttribIList))
            continue;

        if (piFormats != NULL && match_count < nMaxFormats)
            piFormats[match_count] = i;

        match_count++;
    }

    *nNumFormats = match_count;
    (void)hdc;
    return TRUE;
}

#define WGL_EXTENSION_ENTRY(P) { #P, (PROC) P }

static const struct wgl_extension_entry wgl_extension_entries[] = {

   /* WGL_ARB_extensions_string */
   WGL_EXTENSION_ENTRY( wglGetExtensionsStringARB ),

   /* WGL_ARB_pixel_format */
   WGL_EXTENSION_ENTRY( wglChoosePixelFormatARB ),
   WGL_EXTENSION_ENTRY( wglGetPixelFormatAttribfvARB ),
   WGL_EXTENSION_ENTRY( wglGetPixelFormatAttribivARB ),

   /* WGL_ARB_create_context */
   WGL_EXTENSION_ENTRY( wglCreateContextAttribsARB ),

   { NULL, NULL }
};

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

    pfd_table[pfd_count].cColorBits = rbits + gbits + bbits;
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
    struct windrv_context_state *context = windrv_alloc_context(hdc, PFD_MAIN_PLANE);

    if (context == NULL)
        return (DHGLRC)0;
    return context->handle;
}

DHGLRC APIENTRY DrvCreateLayerContext(HDC hdc, INT iLayerPlane)
{
    struct windrv_context_state *context = windrv_alloc_context(hdc, iLayerPlane);

    if (context == NULL)
        return (DHGLRC)0;
    return context->handle;
}

BOOL APIENTRY DrvDeleteContext(DHGLRC dhglrc)
{
    return windrv_free_context(dhglrc);
}

BOOL APIENTRY DrvCopyContext(DHGLRC dhrcSource, DHGLRC dhrcDest, UINT fuMask)
{
    (void)dhrcSource;
    (void)dhrcDest;
    (void)fuMask;
    return TRUE;
}

PGLCLTPROCTABLE APIENTRY DrvSetContext(HDC hdc, DHGLRC dhglrc, PFN_SETPROCTABLE pfnSetProcTable)
{
    struct windrv_context_state *context = windrv_lookup_context(dhglrc);

    if (context == NULL)
        return NULL;

    (void)hdc;
    (void)pfnSetProcTable;
    return glimpl_GetProcTable();
}

BOOL APIENTRY DrvReleaseContext(DHGLRC dhglrc)
{
    (void)dhglrc;
    return TRUE;
}

BOOL APIENTRY DrvValidateVersion(ULONG ulVersion)
{
    return TRUE;
}

BOOL APIENTRY DrvShareLists(DHGLRC dhglrc1, DHGLRC dhglrc2)
{
    (void)dhglrc1;
    (void)dhglrc2;
    return TRUE;
}

PROC APIENTRY DrvGetProcAddress(LPCSTR lpszProc)
{
    PROC proc = NULL;

    if (lpszProc[0] == 'g' && lpszProc[1] == 'l')
        proc = GetProcAddress(module_handle, lpszProc);

    if (!proc && lpszProc[0] == 'w' && lpszProc[1] == 'g' && lpszProc[2] == 'l')
        for (const struct wgl_extension_entry *entry = wgl_extension_entries; entry->name; entry++)
            if (strcmp(lpszProc, entry->name) == 0)
                proc = entry->proc;

    return proc;
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

        swap_sync_lock = pb_ptr(SGL_OFFSET_REGISTER_SWAP_BUFFERS_SYNC);
        framebuffer = glimpl_fb_address();
        init = 1;
    }

    spin_lock(swap_sync_lock);

    glimpl_swap_buffers(real_width, real_height, do_vflip, GL_BGRA); /* to-do: fix overlay so vflip and -Height won't be needed */
    SetDIBitsToDevice(hdc, 0, 0, real_width, real_height, 0, 0, 0, real_height, framebuffer, &bmi, DIB_RGB_COLORS);
    // StretchDIBits(hdc, 0, 0, real_width, real_height, 0, 0, real_width, real_height, framebuffer, &bmi, DIB_RGB_COLORS, SRCCOPY);

    spin_unlock(swap_sync_lock);

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

    (void)hdc;
    return pfd_count;
}

BOOL APIENTRY DrvSetPixelFormat(HDC hdc, LONG iPixelFormat)
{
    if (GetPixelFormat(hdc) == 0)
        SetPixelFormat(hdc, iPixelFormat, NULL);

    return TRUE;
}

// BOOL APIENTRY DrvMakeCurrent(HDC hDC, HGLRC hRC)
// {
//     return TRUE;
// }

#endif
