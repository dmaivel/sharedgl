#include <sharedgl.h>

#include <client/platform/glx.h>
#include <client/glimpl.h>
#include <client/hook.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void *x11 = NULL;

/*
 * to-do: multiple windows
 */
Window win = -1;

static void check_x()
{
    if (!x11)
        x11 = real_dlopen("libX11.so.6", 1);
}

struct glx_swap_data {
    XVisualInfo vinfo;
    XVisualInfo *visual_list;
    XVisualInfo visual_template;
    int nxvisuals;
    Window parent;

    int width, height;
    XImage *ximage;
    XEvent event;

    XGCValues gcv;
    unsigned long gcm;
    GC NormalGC;

    bool initialized;
};

Window XCreateWindow(
    Display* display,
    Window parent,
    int	 x,
    int	 y,
    unsigned int width,
    unsigned int height,
    unsigned int border_width,
    int depth,
    unsigned int class,
    Visual* visual,
    unsigned long valuemask,
    XSetWindowAttributes* attributes
)
{
    check_x();

    Window (*XCreateWindow)(
        Display* display,
        Window parent,
        int	 x,
        int	 y,
        unsigned int width,
        unsigned int height,
        unsigned int border_width,
        int depth,
        unsigned int class,
        Visual* visual,
        unsigned long valuemask,
        XSetWindowAttributes* attributes
    ) = real_dlsym(x11, "XCreateWindow");

    Window w = XCreateWindow(display, parent, x, y, width, height, border_width, depth, class, visual, valuemask, attributes);
    if (width > 1 || class == InputOutput)
        win = w;
    return w;
}

void glXSwapBuffers(Display* dpy, GLXDrawable drawable)
{
    static struct glx_swap_data swap_data = { 0 };
    check_x();

    if (win == -1) {
        return;
    }

    if (swap_data.initialized == false) {
        XWindowAttributes attr;
        XGetWindowAttributes(dpy, win, &attr);

        swap_data.width = attr.width;
        swap_data.height = attr.height;
        swap_data.parent = XDefaultRootWindow(dpy);

        swap_data.nxvisuals = 0;
        swap_data.visual_template.screen = DefaultScreen(dpy);
        swap_data.visual_list = XGetVisualInfo(dpy, VisualScreenMask, &swap_data.visual_template, &swap_data.nxvisuals);

        XMatchVisualInfo(dpy, XDefaultScreen(dpy), 24, TrueColor, &swap_data.vinfo);

        swap_data.ximage = XCreateImage(dpy, swap_data.vinfo.visual, swap_data.vinfo.depth, ZPixmap, 0, glimpl_fb_address(), swap_data.width, swap_data.height, 8, swap_data.width*4);
    
        swap_data.gcm = GCGraphicsExposures;
        swap_data.gcv.graphics_exposures = 0;
        swap_data.NormalGC = XCreateGC(dpy, swap_data.parent, swap_data.gcm, &swap_data.gcv);

        swap_data.initialized = true;
    }

    /* swap */
    glimpl_swap_buffers(swap_data.width, swap_data.height, 1, GL_BGRA);

    /* display */
    XPutImage(dpy, win, swap_data.NormalGC, swap_data.ximage, 0, 0, 0, 0, swap_data.width, swap_data.height);
}

void *glXGetProcAddress(char *s);
void* glXGetProcAddressARB(char* s) 
{
    char str[64];
    if (strstr(s, "EXT") || strstr(s, "ARB")) {
        strcpy(str, s);
        str[strlen(str) - 3] = 0;
        return NULL;
    }

    /* to-do: use above str? */
    return real_dlsym(NULL, s);
}

void *glXGetProcAddress(char *s)
{
    return glXGetProcAddressARB(s);
}