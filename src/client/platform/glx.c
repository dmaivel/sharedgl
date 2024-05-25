#include <sharedgl.h>

#include <client/platform/glx.h>
#include <client/glimpl.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <dlfcn.h>

static Window win = -1;
static const char *glx_extensions = "GLX_ARB_create_context_profile GLX_ARB_get_proc_address GLX_EXT_visual_info";

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

struct glx_fb_config {
    int render_type,
        drawable_type,
        double_buffer,
        red_size,
        green_size,
        blue_size,
        alpha_size,
        stencil_size,
        depth_size,
        accum_red_size,
        accum_green_size,
        accum_blue_size,
        accum_alpha_size,
        accum_stencil_size,
        renderable,
        visual_type;
        // to-do: add more like stereo
};

int n_valid_fb_configs;

static struct glx_fb_config fb_configs[1729] = { 0 };

int fb_valid_color_sizes[] = {
    0, 1, 8, 16, 24, 32
};

int fb_valid_render_types[] = {
    GLX_RGBA_BIT, GLX_COLOR_INDEX_BIT
};

int fb_valid_doublebuffer_types[] = {
    False, True
};

int fb_valid_drawable_types[] = {
    GLX_WINDOW_BIT
};

int fb_valid_visual_types[] = {
    GLX_TRUE_COLOR, GLX_DIRECT_COLOR, GLX_PSEUDO_COLOR, GLX_STATIC_COLOR, GLX_GRAY_SCALE, GLX_STATIC_GRAY
};

#define ARR_SIZE(x) (sizeof(x) / sizeof(*x))
static void glx_generate_fb_configs()
{
    int idx = 0;
    for (int a = 0; a < ARR_SIZE(fb_valid_color_sizes); a++) {                          // varying color size
        for (int b = 0; b < ARR_SIZE(fb_valid_render_types); b++) {                     // varying render types
            for (int c = 0; c < ARR_SIZE(fb_valid_doublebuffer_types); c++) {           // varying double buffer
                for (int d = 0; d < ARR_SIZE(fb_valid_drawable_types); d++) {           // varying drawable type
                    for (int e = 0; e < ARR_SIZE(fb_valid_doublebuffer_types); e++) {   // varying renderable
                        for (int f = 0; f < ARR_SIZE(fb_valid_visual_types); f++) {     // varying visual type
                            for (int g = 0; g < ARR_SIZE(fb_valid_color_sizes); g++) {  // varying depth
                                fb_configs[idx].red_size = fb_valid_color_sizes[a];
                                fb_configs[idx].green_size = fb_valid_color_sizes[a];
                                fb_configs[idx].blue_size = fb_valid_color_sizes[a];
                                fb_configs[idx].alpha_size = fb_valid_color_sizes[a];
                                fb_configs[idx].accum_red_size = fb_valid_color_sizes[a];
                                fb_configs[idx].accum_green_size = fb_valid_color_sizes[a];
                                fb_configs[idx].accum_blue_size = fb_valid_color_sizes[a];
                                fb_configs[idx].accum_alpha_size = fb_valid_color_sizes[a];

                                fb_configs[idx].stencil_size = -1; // fb_valid_color_sizes[a];
                                fb_configs[idx].depth_size = fb_valid_color_sizes[g];

                                fb_configs[idx].render_type = fb_valid_render_types[b];
                                fb_configs[idx].double_buffer = fb_valid_doublebuffer_types[c];
                                fb_configs[idx].drawable_type = fb_valid_drawable_types[d];

                                fb_configs[idx].renderable = fb_valid_doublebuffer_types[e];
                                fb_configs[idx].visual_type = fb_valid_visual_types[f];

                                idx++;
                            }
                        }
                    }
                }
            }
        }
    }

    n_valid_fb_configs = idx;
}
#undef ARR_SIZE

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
    unsigned int index = (int)(size_t)config;
    if (index > n_valid_fb_configs) {
        fprintf(stderr, "glXGetFBConfigAttrib : attempted access on config %u, only %u exist\n", index, n_valid_fb_configs);
        return Success;
    }

    // fprintf(stderr, "glXGetFBConfigAttrib : access on config %u\n", index);

    struct glx_fb_config fb_config = fb_configs[index];

    switch (attribute) {
    case GLX_FBCONFIG_ID:
        *value = index;
        break;
    case GLX_RENDER_TYPE:
        *value |= fb_config.render_type; // GLX_RGBA_BIT;
        break;
    case GLX_VISUAL_ID:
        *value = XDefaultVisual(dpy, 0)->visualid; // fb_config.visual_type;
        break;
    case GLX_BUFFER_SIZE:
        *value = 1;
        break;
    case GLX_SAMPLES:
        *value = 32;
        break;
    case GLX_SAMPLE_BUFFERS:
        *value = 1;
        break;
    case GLX_DRAWABLE_TYPE:
        *value |= fb_config.drawable_type; // GLX_WINDOW_BIT;
        break;
    case GLX_DOUBLEBUFFER:
        *value = fb_config.double_buffer;
        break;
    case GLX_RED_SIZE:
        *value = fb_config.red_size;
        break;
    case GLX_GREEN_SIZE:
        *value = fb_config.green_size;
        break;
    case GLX_BLUE_SIZE:
        *value = fb_config.blue_size;
        break;
    case GLX_ALPHA_SIZE:
        *value = fb_config.alpha_size;
        break;
    case GLX_STENCIL_SIZE:
        *value = fb_config.stencil_size;
        break;
    case GLX_ACCUM_RED_SIZE:
        *value = fb_config.accum_red_size;
        break;
    case GLX_ACCUM_GREEN_SIZE:
        *value = fb_config.accum_green_size;
        break;
    case GLX_ACCUM_BLUE_SIZE:
        *value = fb_config.accum_blue_size;
        break;
    case GLX_ACCUM_ALPHA_SIZE:
        *value = fb_config.accum_alpha_size; // 8;
        break;
    case GLX_DEPTH_SIZE:
        *value = fb_config.depth_size; // 24;
        break;
    case GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB:
        *value = 1;
        break;
    }

    return Success;
}

GLXFBConfig *glXGetFBConfigs(Display *dpy, int screen, int *nelements)
{
    GLXFBConfig *fb_config = calloc(1, sizeof(GLXFBConfig) * n_valid_fb_configs);
    for (int i = 0; i < n_valid_fb_configs; i++)
        fb_config[i] = (GLXFBConfig)((size_t)i);
    *nelements = n_valid_fb_configs;
    return fb_config;
}

XVisualInfo *glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig config)
{
    XVisualInfo *vinfo = malloc(sizeof(XVisualInfo));
    XMatchVisualInfo(dpy, XDefaultScreen(dpy), 24, TrueColor, vinfo);
    return vinfo;
}

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
    return glXGetFBConfigs(dpy, screen, nitems);
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

int glXQueryContext(Display *dpy, GLXContext ctx, int attribute, int *value)
{
    return Success;
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

        /*
         * create an ximage whose pointer points to our framebuffer
         */
        fbb = glimpl_fb_address();
        swap_data.ximage = XCreateImage(dpy, swap_data.vinfo.visual, swap_data.vinfo.depth, ZPixmap, 0, glimpl_fb_address(), swap_data.width, swap_data.height, 8, swap_data.width*4);
        swap_data.gcv.graphics_exposures = 0;
        swap_data.gc = XCreateGC(dpy, swap_data.parent, GCGraphicsExposures, &swap_data.gcv);

        /*
         * report current window dimensions, initialization done
         */
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

void* glXGetProcAddressARB(char* s) 
{
    char str[64];
    if (strstr(s, "EXT") || strstr(s, "ARB")) {
        strcpy(str, s);
        str[strlen(str) - 3] = 0;
        return NULL;
    }

    /* to-do: use stripped str? */
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

    glx_generate_fb_configs();
    // fprintf(stderr, "glximpl_init: n_valid_fb_configs = %d\n", n_valid_fb_configs);
}