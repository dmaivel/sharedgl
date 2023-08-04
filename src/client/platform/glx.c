#include <X11/Xutil.h>
#include <sharedgl.h>

#include <client/platform/glx.h>
#include <client/glimpl.h>
#include <client/hook.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

Window win = -1;

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

GLXContext glXCreateContext(Display *dpy, XVisualInfo *vis, GLXContext share_list, Bool direct)
{
    return (GLXContext)1;
}

void glXDestroyContext(Display *dpy, GLXContext ctx)
{

}

Bool glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx)
{
    return true;
}

Bool glXQueryExtension(Display *dpy, int *errorb, int *event)
{
    return true;
}

void glXQueryDrawable(Display *dpy, GLXDrawable draw, int attribute, unsigned int *value)
{

}

const char *glXQueryExtensionsString(Display *dpy, int screen)
{
    return "";
}

XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attrib_list)
{
    XVisualInfo *vinfo = malloc(sizeof(XVisualInfo));
    XMatchVisualInfo(dpy, XDefaultScreen(dpy), 24, TrueColor, vinfo);
    return vinfo;
}

GLXContext glXCreateNewContext(Display *dpy, GLXFBConfig config, int render_type, GLXContext share_list, Bool direct)
{
    return (GLXContext)1;
}

Display *glXGetCurrentDisplay(void)
{
    return XOpenDisplay(NULL);
}

Bool glXQueryVersion(Display *dpy, int *maj, int *min)
{
    *maj = 1;
    *min = 4;
    return true;
}

const char *glXGetClientString(Display *dpy, int name)
{
    switch (name) {
    case GLX_VENDOR: return "SharedGL";
    case GLX_VERSION: return "1.4";
    case GLX_EXTENSIONS: return "";
    }
    return "";
}

GLXWindow glXCreateWindow(Display *dpy, GLXFBConfig config, Window win, const int *attrib_list)
{
    return 1;
}

void glXDestroyWindow(Display *dpy, GLXWindow win)
{

}

Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx)
{
    win = drawable;
    return true;
}

void glXSwapBuffers(Display* dpy, GLXDrawable drawable)
{
    static struct glx_swap_data swap_data = { 0 };

    if (drawable != win)
        printf("glXSwapBuffers: Drawable is not logged window, expect undefiend behavior\n");

    if (swap_data.initialized == false) {
        XWindowAttributes attr;
        XGetWindowAttributes(dpy, drawable, &attr);

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

        glimpl_report(swap_data.width, swap_data.height);

        swap_data.initialized = true;
    }

    /* swap */
    glimpl_swap_buffers(swap_data.width, swap_data.height, 1, GL_BGRA);

    /* display */
    XPutImage(dpy, drawable, swap_data.NormalGC, swap_data.ximage, 0, 0, 0, 0, swap_data.width, swap_data.height);
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