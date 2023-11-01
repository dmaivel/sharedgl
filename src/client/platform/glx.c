#include <sharedgl.h>

#include <client/platform/glx.h>
#include <client/glimpl.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <dlfcn.h>

static Window win = -1;
static const char *glx_extensions = "GLX_ARB_create_context_profile GLX_EXT_visual_info";

static int glx_major = 1;
static int glx_minor = 4;
static const char *glx_majmin_string = "1.4";

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
    GC gc;

    bool initialized;
};

static const char *glximpl_name_to_string(int name)
{
    switch (name) {
    case GLX_VENDOR: return "SharedGL";
    case GLX_VERSION: return glx_majmin_string;
    case GLX_EXTENSIONS: return glx_extensions;
    }
    return "?";
}

GLXContext glXCreateContext(Display *dpy, XVisualInfo *vis, GLXContext share_list, Bool direct)
{
    return (GLXContext)1;
}

GLXContext glXGetCurrentContext( void )
{
    return (GLXContext)1;
}

void glXDestroyContext(Display *dpy, GLXContext ctx)
{

}

Bool glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx)
{
    return True;
}

Bool glXQueryExtension(Display *dpy, int *errorb, int *event)
{
    return True;
}

void glXQueryDrawable(Display *dpy, GLXDrawable draw, int attribute, unsigned int *value)
{

}

const char *glXQueryExtensionsString(Display *dpy, int screen)
{
    return glx_extensions;
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
    *maj = glx_major;
    *min = glx_minor;
    return True;
}

const char *glXGetClientString(Display *dpy, int name)
{
    return glximpl_name_to_string(name);
}

GLXWindow glXCreateWindow(Display *dpy, GLXFBConfig config, Window win, const int *attrib_list)
{
    return win;
}

void glXDestroyWindow(Display *dpy, GLXWindow win)
{

}

Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx)
{
    win = drawable;
    return true;
}

int glXGetFBConfigAttrib(Display *dpy, GLXFBConfig config, int attribute, int *value)
{
    switch (attribute) {
    case GLX_RENDER_TYPE:
        *value |= GLX_RGBA_BIT;
        break;
    case GLX_DRAWABLE_TYPE:
        *value |= GLX_WINDOW_BIT;
        break;
    case GLX_DOUBLEBUFFER:
        *value = 1;
        break;
    case GLX_RED_SIZE:
    case GLX_GREEN_SIZE:
    case GLX_BLUE_SIZE:
    case GLX_ALPHA_SIZE:
    case GLX_STENCIL_SIZE:
    case GLX_ACCUM_RED_SIZE:
    case GLX_ACCUM_GREEN_SIZE:
    case GLX_ACCUM_BLUE_SIZE:
    case GLX_ACCUM_ALPHA_SIZE:
        *value = 8;
        break;
    case GLX_DEPTH_SIZE:
        *value = 24;
        break;
    }

    return 1;
}

GLXFBConfig *glXGetFBConfigs(Display *dpy, int screen, int *nelements)
{
    GLXFBConfig *fb_config = malloc(sizeof(GLXFBConfig));
    *nelements = 1;
    return fb_config;
}

XVisualInfo *glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig config)
{
    XVisualInfo *vinfo = malloc(sizeof(XVisualInfo));
    XMatchVisualInfo(dpy, XDefaultScreen(dpy), 24, TrueColor, vinfo);
    return vinfo;
}

/*
 * to-do: to do.
 */
GLXContext glXCreateContextAttribsARB(Display *dpy, GLXFBConfig config, GLXContext share_context, Bool direct, const int *attrib_list)
{
    return glXCreateContext(NULL, NULL, 0, 0);
}

int glXGetConfig(Display *dpy, XVisualInfo *visual, int attrib, int *value)
{
    return 1;
}

GLXFBConfig *glXChooseFBConfig(Display *dpy, int screen, const int *attrib_list, int *nitems)
{
    return malloc(sizeof(GLXFBConfig));
}

Bool glXIsDirect(Display *dpy, GLXContext ctx)
{
    return True;
}

const char *glXQueryServerString( Display *dpy, int screen, int name )
{
    return glximpl_name_to_string(name);
}

void glXCopyContext(Display *dpy, GLXContext src, GLXContext dst, unsigned long mask)
{

}

GLXPixmap glXCreateGLXPixmap(Display *dpy, XVisualInfo *visual, Pixmap pixmap)
{
    return (GLXPixmap)1;
}

void glXDestroyGLXPixmap(Display *dpy, GLXPixmap pixmap)
{

}

GLXDrawable glXGetCurrentDrawable(void)
{
    return win;
}

void glXUseXFont(Font font, int first, int count, int list)
{
    
}

void glXWaitGL(void)
{

}

void glXWaitX(void)
{

}

void glXSwapBuffers(Display* dpy, GLXDrawable drawable)
{
    static struct glx_swap_data swap_data = { 0 };
    static char *fbb = NULL;

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

        fbb = glimpl_fb_address();
        swap_data.ximage = XCreateImage(dpy, swap_data.vinfo.visual, swap_data.vinfo.depth, ZPixmap, 0, glimpl_fb_address(), swap_data.width, swap_data.height, 8, swap_data.width*4);
        swap_data.gcv.graphics_exposures = 0;
        swap_data.gc = XCreateGC(dpy, swap_data.parent, GCGraphicsExposures, &swap_data.gcv);

        glimpl_report(swap_data.width, swap_data.height);

        swap_data.initialized = true;
    }

    /* swap */
    glimpl_swap_buffers(swap_data.width, swap_data.height, 1, GL_BGRA);

    /* display */
    XPutImage(dpy, drawable, swap_data.gc, swap_data.ximage, 0, 0, 0, 0, swap_data.width, swap_data.height);

    /* sync */
    XSync(dpy, False);
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
    return dlsym(NULL, s);
}

void *glXGetProcAddress(char *s)
{
    return glXGetProcAddressARB(s);
}

void glximpl_init()
{
    char *glx_version_override = getenv("GLX_VERSION_OVERRIDE");
    if (glx_version_override) {
        glx_majmin_string = glx_version_override;
        glx_major = glx_version_override[0] - '0';
        glx_minor = glx_version_override[2] - '0';
    }
}