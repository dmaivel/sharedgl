#define _GNU_SOURCE

#include <sharedgl.h>

#include <client/platform/icd.h>
#include <client/platform/glx.h>
#include <client/glimpl.h>

#include <client/pb.h>
#include <client/spinlock.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>

static const char *glx_extensions =
    "GLX_ARB_create_context "
    "GLX_ARB_create_context_no_error "
    "GLX_ARB_create_context_profile "
    "GLX_ARB_create_context_robustness "
    "GLX_ARB_get_proc_address "
    "GLX_EXT_visual_info";

static int glx_major = 1;
static int glx_minor = 4;
static const char *glx_majmin_string = "1.4";

static int max_width, max_height, real_width, real_height;
static void *swap_sync_lock;

Bool glXQueryVersion(Display *dpy, int *maj, int *min);
GLXContext glXCreateContextAttribsARB(Display *dpy, GLXFBConfig config, GLXContext share_context, Bool direct, const int *attrib_list);

ICD_SET_MAX_DIMENSIONS_DEFINITION(max_width, max_height, real_width, real_height);
ICD_RESIZE_DEFINITION(real_width, real_height);

#define GLX_MAX_CONTEXTS 64
#define GLX_MAX_DRAWABLES 64
#define GLX_MAX_FB_CONFIGS 8

struct glx_swap_data {
    Display *display;
    GLXDrawable drawable;
    XImage *ximage;
    GC gc;
    bool initialized;
};

struct glx_fb_config {
    int render_type;
    int drawable_type;
    int double_buffer;
    int red_size;
    int green_size;
    int blue_size;
    int alpha_size;
    int stencil_size;
    int depth_size;
    int accum_red_size;
    int accum_green_size;
    int accum_blue_size;
    int accum_alpha_size;
    int renderable;
    int visual_type;
    int sample_buffers;
    int samples;
};

struct glx_context_state {
    GLXFBConfig config;
    int render_type;
    int screen;
    int major;
    int minor;
    int flags;
    int profile_mask;
    int reset_notification_strategy;
    int release_behavior;
    Bool direct;
};

struct glx_drawable_state {
    bool in_use;
    Display *display;
    GLXDrawable drawable;
    GLXFBConfig config;
    int screen;
};

static GLXContext current_context = NULL;
static Display *current_display = NULL;
static GLXDrawable current_drawable = 0;
static GLXDrawable current_read_drawable = 0;

static struct glx_context_state *glx_contexts[GLX_MAX_CONTEXTS] = { 0 };
static struct glx_drawable_state glx_drawables[GLX_MAX_DRAWABLES] = { 0 };

static int n_valid_fb_configs;
static struct glx_fb_config fb_configs[GLX_MAX_FB_CONFIGS] = { 0 };
static void *glx_self_handle;
static bool glx_self_handle_initialized;

static void *glx_get_self_handle(void)
{
    Dl_info info;

    if (glx_self_handle_initialized)
        return glx_self_handle;

    glx_self_handle_initialized = true;

    if (dladdr((void *)glXQueryVersion, &info) == 0 || info.dli_fname == NULL)
        return NULL;

    glx_self_handle = dlopen(info.dli_fname, RTLD_LAZY | RTLD_LOCAL);
    if (glx_self_handle == NULL)
        return NULL;

    return glx_self_handle;
}

static void *glx_lookup_builtin_proc(const char *name)
{
    if (name == NULL)
        return NULL;

    if (strcmp(name, "glXCreateContextAttribsARB") == 0)
        return (void *)glXCreateContextAttribsARB;

    return NULL;
}

static void glx_destroy_swap_data(struct glx_swap_data *swap_data)
{
    if (!swap_data->initialized)
        return;

    if (swap_data->gc != None)
        XFreeGC(swap_data->display, swap_data->gc);

    if (swap_data->ximage != NULL) {
        swap_data->ximage->data = NULL;
        XDestroyImage(swap_data->ximage);
    }

    *swap_data = (struct glx_swap_data){ 0 };
}

static void glx_add_fb_config(int alpha_size, int depth_size, int stencil_size, int double_buffer)
{
    if (n_valid_fb_configs >= GLX_MAX_FB_CONFIGS)
        return;

    fb_configs[n_valid_fb_configs++] = (struct glx_fb_config){
        .render_type = GLX_RGBA_BIT,
        .drawable_type = GLX_WINDOW_BIT,
        .double_buffer = double_buffer,
        .red_size = 8,
        .green_size = 8,
        .blue_size = 8,
        .alpha_size = alpha_size,
        .stencil_size = stencil_size,
        .depth_size = depth_size,
        .accum_red_size = 0,
        .accum_green_size = 0,
        .accum_blue_size = 0,
        .accum_alpha_size = 0,
        .renderable = True,
        .visual_type = GLX_TRUE_COLOR,
        .sample_buffers = 0,
        .samples = 0
    };
}

static void glx_generate_fb_configs(void)
{
    n_valid_fb_configs = 0;

    glx_add_fb_config(8, 24, 8, True);
    glx_add_fb_config(8, 24, 8, False);
    glx_add_fb_config(0, 24, 8, True);
    glx_add_fb_config(0, 24, 8, False);
    glx_add_fb_config(8, 16, 0, True);
    glx_add_fb_config(8, 16, 0, False);
    glx_add_fb_config(0, 16, 0, True);
    glx_add_fb_config(0, 16, 0, False);
}

static GLXFBConfig glx_index_to_fb_config(int index)
{
    return (GLXFBConfig)(uintptr_t)(index + 1);
}

static bool glx_fb_config_to_index(GLXFBConfig config, int *index)
{
    uintptr_t raw = (uintptr_t)config;

    if (raw == 0 || raw > (uintptr_t)n_valid_fb_configs)
        return false;

    *index = (int)raw - 1;
    return true;
}

static int glx_fb_config_buffer_size(const struct glx_fb_config *fb_config)
{
    return fb_config->red_size + fb_config->green_size + fb_config->blue_size + fb_config->alpha_size;
}

static const struct glx_fb_config *glx_default_fb_config(void)
{
    return &fb_configs[0];
}

static XVisualInfo *glx_allocate_visual_info(Display *dpy, int screen, const struct glx_fb_config *fb_config)
{
    XVisualInfo visual_template = {
        .visualid = XVisualIDFromVisual(DefaultVisual(dpy, screen))
    };
    XVisualInfo *vinfo;
    int preferred_depths[3];
    int n_depths = 0;
    int n_visuals = 0;

    vinfo = XGetVisualInfo(dpy, VisualIDMask, &visual_template, &n_visuals);
    if (vinfo != NULL && n_visuals > 0 && vinfo->screen == screen && vinfo->class == TrueColor) {
        return vinfo;
    }
    if (vinfo != NULL)
        XFree(vinfo);

    vinfo = malloc(sizeof(*vinfo));
    if (vinfo == NULL)
        return NULL;

    preferred_depths[n_depths++] = DefaultDepth(dpy, screen);

    if (preferred_depths[0] != 24)
        preferred_depths[n_depths++] = 24;

    if (fb_config != NULL && fb_config->alpha_size > 0 && preferred_depths[0] != 32)
        preferred_depths[n_depths++] = 32;

    for (int i = 0; i < n_depths; i++) {
        if (XMatchVisualInfo(dpy, screen, preferred_depths[i], TrueColor, vinfo))
            return vinfo;
    }

    free(vinfo);
    return NULL;
}

static XVisualInfo *glx_allocate_visual_for_config(Display *dpy, int screen, GLXFBConfig config)
{
    int index;

    if (glx_fb_config_to_index(config, &index))
        return glx_allocate_visual_info(dpy, screen, &fb_configs[index]);

    return glx_allocate_visual_info(dpy, screen, glx_default_fb_config());
}

static bool glx_is_legacy_boolean_attr(int attribute)
{
    switch (attribute) {
    case GLX_USE_GL:
    case GLX_RGBA:
    case GLX_DOUBLEBUFFER:
    case GLX_STEREO:
        return true;
    default:
        return false;
    }
}

static bool glx_match_fb_config_attribute(const struct glx_fb_config *fb_config, int screen, int attribute, int value)
{
    switch (attribute) {
    case GLX_X_RENDERABLE:
        return value == GLX_DONT_CARE || fb_config->renderable == !!value;
    case GLX_DRAWABLE_TYPE:
        return value == GLX_DONT_CARE || (fb_config->drawable_type & value) == value;
    case GLX_RENDER_TYPE:
        return value == GLX_DONT_CARE || (fb_config->render_type & value) == value;
    case GLX_X_VISUAL_TYPE:
        return value == GLX_DONT_CARE || fb_config->visual_type == value;
    case GLX_CONFIG_CAVEAT:
        return value == GLX_DONT_CARE || value == GLX_NONE;
    case GLX_TRANSPARENT_TYPE:
        return value == GLX_DONT_CARE || value == GLX_NONE;
    case GLX_BUFFER_SIZE:
        return value == GLX_DONT_CARE || glx_fb_config_buffer_size(fb_config) >= value;
    case GLX_LEVEL:
        return value == GLX_DONT_CARE || value == 0;
    case GLX_DOUBLEBUFFER:
        return value == GLX_DONT_CARE || fb_config->double_buffer == !!value;
    case GLX_STEREO:
        return value == GLX_DONT_CARE || value == False;
    case GLX_AUX_BUFFERS:
        return value == GLX_DONT_CARE || value <= 0;
    case GLX_RED_SIZE:
        return value == GLX_DONT_CARE || fb_config->red_size >= value;
    case GLX_GREEN_SIZE:
        return value == GLX_DONT_CARE || fb_config->green_size >= value;
    case GLX_BLUE_SIZE:
        return value == GLX_DONT_CARE || fb_config->blue_size >= value;
    case GLX_ALPHA_SIZE:
        return value == GLX_DONT_CARE || fb_config->alpha_size >= value;
    case GLX_DEPTH_SIZE:
        return value == GLX_DONT_CARE || fb_config->depth_size >= value;
    case GLX_STENCIL_SIZE:
        return value == GLX_DONT_CARE || fb_config->stencil_size >= value;
    case GLX_ACCUM_RED_SIZE:
        return value == GLX_DONT_CARE || fb_config->accum_red_size >= value;
    case GLX_ACCUM_GREEN_SIZE:
        return value == GLX_DONT_CARE || fb_config->accum_green_size >= value;
    case GLX_ACCUM_BLUE_SIZE:
        return value == GLX_DONT_CARE || fb_config->accum_blue_size >= value;
    case GLX_ACCUM_ALPHA_SIZE:
        return value == GLX_DONT_CARE || fb_config->accum_alpha_size >= value;
    case GLX_SAMPLE_BUFFERS:
        return value == GLX_DONT_CARE || fb_config->sample_buffers >= value;
    case GLX_SAMPLES:
        return value == GLX_DONT_CARE || fb_config->samples >= value;
    case GLX_FBCONFIG_ID:
        return value == GLX_DONT_CARE ||
               value == (int)(uintptr_t)glx_index_to_fb_config((int)(fb_config - fb_configs));
    case GLX_SCREEN:
        return value == GLX_DONT_CARE || value == screen;
    case GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB:
        return value == GLX_DONT_CARE || value == True;
    default:
        return true;
    }
}

static bool glx_fb_config_matches(const struct glx_fb_config *fb_config, int screen, const int *attrib_list)
{
    const int *attrib = attrib_list;

    while (attrib != NULL && *attrib != None) {
        int key = *attrib++;

        if (*attrib == None)
            return true;

        if (!glx_match_fb_config_attribute(fb_config, screen, key, *attrib++))
            return false;
    }

    return true;
}

static bool glx_visual_config_matches(const struct glx_fb_config *fb_config, const int *attrib_list)
{
    const int *attrib = attrib_list;

    while (attrib != NULL && *attrib != None) {
        int key = *attrib++;
        int value = 0;

        if (!glx_is_legacy_boolean_attr(key)) {
            if (*attrib == None)
                return true;
            value = *attrib++;
        }

        switch (key) {
        case GLX_USE_GL:
        case GLX_RGBA:
            if ((fb_config->render_type & GLX_RGBA_BIT) == 0)
                return false;
            break;
        case GLX_DOUBLEBUFFER:
            if (!fb_config->double_buffer)
                return false;
            break;
        case GLX_STEREO:
            return false;
        case GLX_BUFFER_SIZE:
            if (glx_fb_config_buffer_size(fb_config) < value)
                return false;
            break;
        case GLX_LEVEL:
            if (value != 0)
                return false;
            break;
        case GLX_AUX_BUFFERS:
            if (value > 0)
                return false;
            break;
        case GLX_RED_SIZE:
            if (fb_config->red_size < value)
                return false;
            break;
        case GLX_GREEN_SIZE:
            if (fb_config->green_size < value)
                return false;
            break;
        case GLX_BLUE_SIZE:
            if (fb_config->blue_size < value)
                return false;
            break;
        case GLX_ALPHA_SIZE:
            if (fb_config->alpha_size < value)
                return false;
            break;
        case GLX_DEPTH_SIZE:
            if (fb_config->depth_size < value)
                return false;
            break;
        case GLX_STENCIL_SIZE:
            if (fb_config->stencil_size < value)
                return false;
            break;
        case GLX_ACCUM_RED_SIZE:
            if (fb_config->accum_red_size < value)
                return false;
            break;
        case GLX_ACCUM_GREEN_SIZE:
            if (fb_config->accum_green_size < value)
                return false;
            break;
        case GLX_ACCUM_BLUE_SIZE:
            if (fb_config->accum_blue_size < value)
                return false;
            break;
        case GLX_ACCUM_ALPHA_SIZE:
            if (fb_config->accum_alpha_size < value)
                return false;
            break;
        default:
            break;
        }
    }

    return true;
}

static struct glx_context_state *glx_lookup_context(GLXContext ctx)
{
    struct glx_context_state *context = (struct glx_context_state *)ctx;

    for (int i = 0; i < GLX_MAX_CONTEXTS; i++) {
        if (glx_contexts[i] == context)
            return context;
    }

    return NULL;
}

static bool glx_register_context(struct glx_context_state *context)
{
    for (int i = 0; i < GLX_MAX_CONTEXTS; i++) {
        if (glx_contexts[i] == NULL) {
            glx_contexts[i] = context;
            return true;
        }
    }

    return false;
}

static void glx_unregister_context(struct glx_context_state *context)
{
    for (int i = 0; i < GLX_MAX_CONTEXTS; i++) {
        if (glx_contexts[i] == context) {
            glx_contexts[i] = NULL;
            return;
        }
    }
}

static struct glx_context_state *glx_create_context_state(GLXFBConfig config, int render_type, int screen, Bool direct)
{
    struct glx_context_state *context = calloc(1, sizeof(*context));

    if (context == NULL)
        return NULL;

    context->config = config;
    context->render_type = render_type;
    context->screen = screen;
    context->major = 1;
    context->minor = 0;
    context->profile_mask = GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
    context->reset_notification_strategy = GLX_NO_RESET_NOTIFICATION_ARB;
    context->release_behavior = GLX_CONTEXT_RELEASE_BEHAVIOR_FLUSH_ARB;
    context->direct = direct;

    if (!glx_register_context(context)) {
        free(context);
        return NULL;
    }

    return context;
}

static struct glx_drawable_state *glx_find_drawable_state(Display *dpy, GLXDrawable drawable)
{
    for (int i = 0; i < GLX_MAX_DRAWABLES; i++) {
        if (glx_drawables[i].in_use && glx_drawables[i].display == dpy && glx_drawables[i].drawable == drawable)
            return &glx_drawables[i];
    }

    return NULL;
}

static void glx_track_drawable(Display *dpy, GLXDrawable drawable, GLXFBConfig config, int screen)
{
    struct glx_drawable_state *state = glx_find_drawable_state(dpy, drawable);

    if (state == NULL) {
        for (int i = 0; i < GLX_MAX_DRAWABLES; i++) {
            if (!glx_drawables[i].in_use) {
                state = &glx_drawables[i];
                break;
            }
        }
    }

    if (state == NULL)
        return;

    *state = (struct glx_drawable_state){
        .in_use = true,
        .display = dpy,
        .drawable = drawable,
        .config = config,
        .screen = screen
    };
}

static void glx_untrack_drawable(Display *dpy, GLXDrawable drawable)
{
    struct glx_drawable_state *state = glx_find_drawable_state(dpy, drawable);

    if (state != NULL)
        *state = (struct glx_drawable_state){ 0 };
}

static const char *glximpl_name_to_string(int name)
{
    switch (name) {
    case GLX_VENDOR:
        return "SharedGL";
    case GLX_VERSION:
        return glx_majmin_string;
    case GLX_EXTENSIONS:
        return glx_extensions;
    default:
        return "?";
    }
}

GLXContext glXCreateContext(Display *dpy, XVisualInfo *vis, GLXContext share_list, Bool direct)
{
    int screen = vis != NULL ? vis->screen : DefaultScreen(dpy);

    (void)share_list;

    return (GLXContext)glx_create_context_state(glx_index_to_fb_config(0), GLX_RGBA_TYPE, screen, direct);
}

GLXContext glXGetCurrentContext(void)
{
    return current_context;
}

void glXDestroyContext(Display *dpy, GLXContext ctx)
{
    struct glx_context_state *context = glx_lookup_context(ctx);

    (void)dpy;

    if (context == NULL)
        return;

    if (current_context == ctx) {
        current_context = NULL;
        current_display = NULL;
        current_drawable = 0;
        current_read_drawable = 0;
    }

    glx_unregister_context(context);
    free(context);
}

Bool glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx)
{
    if (ctx == NULL) {
        current_context = NULL;
        current_display = NULL;
        current_drawable = 0;
        current_read_drawable = 0;
        return True;
    }

    if (glx_lookup_context(ctx) == NULL)
        return False;

    current_context = ctx;
    current_display = dpy;
    current_drawable = draw;
    current_read_drawable = read;
    return True;
}

Bool glXQueryExtension(Display *dpy, int *errorb, int *event)
{
    (void)dpy;

    if (errorb != NULL)
        *errorb = 0;
    if (event != NULL)
        *event = 0;

    return True;
}

void glXQueryDrawable(Display *dpy, GLXDrawable draw, int attribute, unsigned int *value)
{
    XWindowAttributes attr;
    struct glx_drawable_state *state;

    if (value == NULL)
        return;

    *value = 0;
    state = glx_find_drawable_state(dpy, draw);

    switch (attribute) {
    case GLX_WIDTH:
    case GLX_HEIGHT:
        if (XGetWindowAttributes(dpy, draw, &attr))
            *value = (attribute == GLX_WIDTH) ? (unsigned int)attr.width : (unsigned int)attr.height;
        break;
    case GLX_EVENT_MASK:
        *value = 0;
        break;
    case GLX_FBCONFIG_ID:
        if (state != NULL) {
            *value = (unsigned int)(uintptr_t)state->config;
        } else if (current_context != NULL && current_drawable == draw) {
            struct glx_context_state *context = glx_lookup_context(current_context);
            if (context != NULL)
                *value = (unsigned int)(uintptr_t)context->config;
        }
        break;
    case GLX_PRESERVED_CONTENTS:
    case GLX_LARGEST_PBUFFER:
        *value = False;
        break;
    default:
        break;
    }
}

const char *glXQueryExtensionsString(Display *dpy, int screen)
{
    (void)dpy;
    (void)screen;
    return glx_extensions;
}

XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attrib_list)
{
    for (int i = 0; i < n_valid_fb_configs; i++) {
        if (glx_visual_config_matches(&fb_configs[i], attrib_list)) {
            XVisualInfo *vinfo = glx_allocate_visual_info(dpy, screen, &fb_configs[i]);
            return vinfo;
        }
    }

    return NULL;
}

GLXContext glXCreateNewContext(Display *dpy, GLXFBConfig config, int render_type, GLXContext share_list, Bool direct)
{
    int index;

    (void)share_list;

    if (!glx_fb_config_to_index(config, &index))
        return NULL;

    if (render_type == GLX_RGBA_TYPE) {
        if ((fb_configs[index].render_type & GLX_RGBA_BIT) == 0)
            return NULL;
    } else if (render_type == GLX_COLOR_INDEX_TYPE) {
        if ((fb_configs[index].render_type & GLX_COLOR_INDEX_BIT) == 0)
            return NULL;
    } else {
        return NULL;
    }

    return (GLXContext)glx_create_context_state(config, render_type, DefaultScreen(dpy), direct);
}

Display *glXGetCurrentDisplay(void)
{
    return current_display;
}

Bool glXQueryVersion(Display *dpy, int *maj, int *min)
{
    (void)dpy;

    if (maj != NULL)
        *maj = glx_major;
    if (min != NULL)
        *min = glx_minor;

    return True;
}

const char *glXGetClientString(Display *dpy, int name)
{
    (void)dpy;
    return glximpl_name_to_string(name);
}

GLXWindow glXCreateWindow(Display *dpy, GLXFBConfig config, Window win, const int *attrib_list)
{
    int index;

    (void)attrib_list;

    if (!glx_fb_config_to_index(config, &index))
        return 0;

    glx_track_drawable(dpy, win, config, DefaultScreen(dpy));
    return win;
}

void glXDestroyWindow(Display *dpy, GLXWindow win)
{
    glx_untrack_drawable(dpy, win);
}

Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx)
{
    return glXMakeContextCurrent(dpy, drawable, drawable, ctx);
}

int glXGetFBConfigAttrib(Display *dpy, GLXFBConfig config, int attribute, int *value)
{
    int index;
    XVisualInfo *vinfo;
    const struct glx_fb_config *fb_config;

    if (value == NULL)
        return GLX_BAD_VALUE;

    if (!glx_fb_config_to_index(config, &index))
        return GLX_BAD_VALUE;

    *value = 0;
    fb_config = &fb_configs[index];

    switch (attribute) {
    case GLX_FBCONFIG_ID:
        *value = (int)(uintptr_t)config;
        break;
    case GLX_RENDER_TYPE:
        *value = fb_config->render_type;
        break;
    case GLX_VISUAL_ID:
        vinfo = glx_allocate_visual_for_config(dpy, DefaultScreen(dpy), config);
        if (vinfo == NULL)
            return GLX_BAD_VALUE;
        *value = (int)vinfo->visualid;
        free(vinfo);
        break;
    case GLX_BUFFER_SIZE:
        *value = glx_fb_config_buffer_size(fb_config);
        break;
    case GLX_SAMPLES:
        *value = fb_config->samples;
        break;
    case GLX_SAMPLE_BUFFERS:
        *value = fb_config->sample_buffers;
        break;
    case GLX_DRAWABLE_TYPE:
        *value = fb_config->drawable_type;
        break;
    case GLX_DOUBLEBUFFER:
        *value = fb_config->double_buffer;
        break;
    case GLX_RED_SIZE:
        *value = fb_config->red_size;
        break;
    case GLX_GREEN_SIZE:
        *value = fb_config->green_size;
        break;
    case GLX_BLUE_SIZE:
        *value = fb_config->blue_size;
        break;
    case GLX_ALPHA_SIZE:
        *value = fb_config->alpha_size;
        break;
    case GLX_STENCIL_SIZE:
        *value = fb_config->stencil_size;
        break;
    case GLX_ACCUM_RED_SIZE:
        *value = fb_config->accum_red_size;
        break;
    case GLX_ACCUM_GREEN_SIZE:
        *value = fb_config->accum_green_size;
        break;
    case GLX_ACCUM_BLUE_SIZE:
        *value = fb_config->accum_blue_size;
        break;
    case GLX_ACCUM_ALPHA_SIZE:
        *value = fb_config->accum_alpha_size;
        break;
    case GLX_DEPTH_SIZE:
        *value = fb_config->depth_size;
        break;
    case GLX_X_RENDERABLE:
        *value = fb_config->renderable;
        break;
    case GLX_X_VISUAL_TYPE:
        *value = fb_config->visual_type;
        break;
    case GLX_CONFIG_CAVEAT:
        *value = GLX_NONE;
        break;
    case GLX_TRANSPARENT_TYPE:
        *value = GLX_NONE;
        break;
    case GLX_SCREEN:
        *value = DefaultScreen(dpy);
        break;
    case GLX_LEVEL:
        *value = 0;
        break;
    case GLX_STEREO:
        *value = False;
        break;
    case GLX_AUX_BUFFERS:
        *value = 0;
        break;
    case GLX_MAX_PBUFFER_WIDTH:
    case GLX_MAX_PBUFFER_HEIGHT:
    case GLX_MAX_PBUFFER_PIXELS:
        *value = 0;
        break;
    case GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB:
        *value = True;
        break;
    default:
        return GLX_BAD_ATTRIBUTE;
    }

    return Success;
}

GLXFBConfig *glXGetFBConfigs(Display *dpy, int screen, int *nelements)
{
    GLXFBConfig *configs = calloc((size_t)n_valid_fb_configs, sizeof(GLXFBConfig));

    (void)dpy;
    (void)screen;

    if (nelements != NULL)
        *nelements = n_valid_fb_configs;

    if (configs == NULL)
        return NULL;

    for (int i = 0; i < n_valid_fb_configs; i++)
        configs[i] = glx_index_to_fb_config(i);

    return configs;
}

XVisualInfo *glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig config)
{
    int index;
    XVisualInfo *vinfo;

    if (!glx_fb_config_to_index(config, &index))
        return NULL;

    return glx_allocate_visual_for_config(dpy, DefaultScreen(dpy), config);
}

GLXContext glXCreateContextAttribsARB(Display *dpy, GLXFBConfig config, GLXContext share_context, Bool direct, const int *attrib_list)
{
    int index;
    struct glx_context_state *context;
    const int *attrib = attrib_list;

    if (!glx_fb_config_to_index(config, &index))
        return NULL;

    if (share_context != NULL && glx_lookup_context(share_context) == NULL)
        return NULL;

    context = glx_create_context_state(config, GLX_RGBA_TYPE, DefaultScreen(dpy), direct);
    if (context == NULL)
        return NULL;

    while (attrib != NULL && *attrib != None) {
        int key = *attrib++;

        if (*attrib == None)
            break;

        switch (key) {
        case GLX_CONTEXT_MAJOR_VERSION_ARB:
            context->major = *attrib;
            break;
        case GLX_CONTEXT_MINOR_VERSION_ARB:
            context->minor = *attrib;
            break;
        case GLX_CONTEXT_FLAGS_ARB:
            context->flags = *attrib;
            break;
        case GLX_CONTEXT_PROFILE_MASK_ARB:
            context->profile_mask = *attrib;
            if (context->profile_mask & GLX_CONTEXT_ES2_PROFILE_BIT_EXT) {
                glXDestroyContext(dpy, (GLXContext)context);
                return NULL;
            }
            break;
        case GLX_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB:
            context->reset_notification_strategy = *attrib;
            break;
        case GLX_CONTEXT_RELEASE_BEHAVIOR_ARB:
            context->release_behavior = *attrib;
            break;
        case GLX_CONTEXT_OPENGL_NO_ERROR_ARB:
            break;
        default:
            break;
        }

        attrib++;
    }

    return (GLXContext)context;
}

int glXGetConfig(Display *dpy, XVisualInfo *visual, int attrib, int *value)
{
    const struct glx_fb_config *fb_config = glx_default_fb_config();

    (void)dpy;
    (void)visual;

    if (value == NULL)
        return GLX_BAD_VALUE;

    switch (attrib) {
    case GLX_USE_GL:
    case GLX_RGBA:
        *value = True;
        break;
    case GLX_BUFFER_SIZE:
        *value = glx_fb_config_buffer_size(fb_config);
        break;
    case GLX_LEVEL:
        *value = 0;
        break;
    case GLX_DOUBLEBUFFER:
        *value = fb_config->double_buffer;
        break;
    case GLX_STEREO:
        *value = False;
        break;
    case GLX_AUX_BUFFERS:
        *value = 0;
        break;
    case GLX_RED_SIZE:
        *value = fb_config->red_size;
        break;
    case GLX_GREEN_SIZE:
        *value = fb_config->green_size;
        break;
    case GLX_BLUE_SIZE:
        *value = fb_config->blue_size;
        break;
    case GLX_ALPHA_SIZE:
        *value = fb_config->alpha_size;
        break;
    case GLX_DEPTH_SIZE:
        *value = fb_config->depth_size;
        break;
    case GLX_STENCIL_SIZE:
        *value = fb_config->stencil_size;
        break;
    case GLX_ACCUM_RED_SIZE:
        *value = fb_config->accum_red_size;
        break;
    case GLX_ACCUM_GREEN_SIZE:
        *value = fb_config->accum_green_size;
        break;
    case GLX_ACCUM_BLUE_SIZE:
        *value = fb_config->accum_blue_size;
        break;
    case GLX_ACCUM_ALPHA_SIZE:
        *value = fb_config->accum_alpha_size;
        break;
    default:
        return GLX_BAD_ATTRIBUTE;
    }

    return Success;
}

GLXFBConfig *glXChooseFBConfig(Display *dpy, int screen, const int *attrib_list, int *nitems)
{
    int matches = 0;
    GLXFBConfig *configs;

    (void)dpy;

    for (int i = 0; i < n_valid_fb_configs; i++) {
        if (glx_fb_config_matches(&fb_configs[i], screen, attrib_list))
            matches++;
    }

    if (nitems != NULL)
        *nitems = matches;

    if (matches == 0)
        return NULL;

    configs = calloc((size_t)matches, sizeof(GLXFBConfig));
    if (configs == NULL)
        return NULL;

    matches = 0;
    for (int i = 0; i < n_valid_fb_configs; i++) {
        if (glx_fb_config_matches(&fb_configs[i], screen, attrib_list))
            configs[matches++] = glx_index_to_fb_config(i);
    }

    return configs;
}

Bool glXIsDirect(Display *dpy, GLXContext ctx)
{
    struct glx_context_state *context = glx_lookup_context(ctx);

    (void)dpy;

    return context != NULL ? context->direct : False;
}

const char *glXQueryServerString(Display *dpy, int screen, int name)
{
    (void)dpy;
    (void)screen;
    return glximpl_name_to_string(name);
}

void glXCopyContext(Display *dpy, GLXContext src, GLXContext dst, unsigned long mask)
{
    (void)dpy;
    (void)src;
    (void)dst;
    (void)mask;
}

GLXPixmap glXCreateGLXPixmap(Display *dpy, XVisualInfo *visual, Pixmap pixmap)
{
    (void)dpy;
    (void)visual;
    return (GLXPixmap)pixmap;
}

void glXDestroyGLXPixmap(Display *dpy, GLXPixmap pixmap)
{
    (void)dpy;
    (void)pixmap;
}

GLXDrawable glXGetCurrentDrawable(void)
{
    return current_drawable;
}

void glXUseXFont(Font font, int first, int count, int list)
{
    (void)font;
    (void)first;
    (void)count;
    (void)list;
}

void glXWaitGL(void)
{
}

void glXWaitX(void)
{
}

int glXQueryContext(Display *dpy, GLXContext ctx, int attribute, int *value)
{
    struct glx_context_state *context = glx_lookup_context(ctx);

    (void)dpy;

    if (context == NULL)
        return GLX_BAD_CONTEXT;

    if (value == NULL)
        return GLX_BAD_VALUE;

    switch (attribute) {
    case GLX_FBCONFIG_ID:
        *value = (int)(uintptr_t)context->config;
        break;
    case GLX_RENDER_TYPE:
        *value = context->render_type;
        break;
    case GLX_SCREEN:
        *value = context->screen;
        break;
    case GLX_CONTEXT_MAJOR_VERSION_ARB:
        *value = context->major;
        break;
    case GLX_CONTEXT_MINOR_VERSION_ARB:
        *value = context->minor;
        break;
    case GLX_CONTEXT_FLAGS_ARB:
        *value = context->flags;
        break;
    case GLX_CONTEXT_PROFILE_MASK_ARB:
        *value = context->profile_mask;
        break;
    case GLX_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB:
        *value = context->reset_notification_strategy;
        break;
    case GLX_CONTEXT_RELEASE_BEHAVIOR_ARB:
        *value = context->release_behavior;
        break;
    default:
        return GLX_BAD_ATTRIBUTE;
    }

    return Success;
}

void glXSwapBuffers(Display *dpy, GLXDrawable drawable)
{
    static struct glx_swap_data swap_data = { 0 };
    XWindowAttributes attr;

    if (dpy == NULL || drawable == 0)
        return;

    if (swap_sync_lock == NULL)
        swap_sync_lock = pb_ptr(SGL_OFFSET_REGISTER_SWAP_BUFFERS_SYNC);

    if (!swap_data.initialized || swap_data.display != dpy || swap_data.drawable != drawable) {
        glx_destroy_swap_data(&swap_data);

        if (!XGetWindowAttributes(dpy, drawable, &attr))
            return;

        swap_data.ximage = XCreateImage(
            dpy,
            attr.visual,
            (unsigned int)attr.depth,
            ZPixmap,
            0,
            glimpl_fb_address(),
            max_width,
            max_height,
            32,
            max_width * 4
        );
        if (swap_data.ximage == NULL)
            return;

        swap_data.gc = XCreateGC(dpy, drawable, 0, NULL);
        if (swap_data.gc == None) {
            swap_data.ximage->data = NULL;
            XDestroyImage(swap_data.ximage);
            swap_data.ximage = NULL;
            return;
        }

        swap_data.display = dpy;
        swap_data.drawable = drawable;
        swap_data.initialized = true;
    }

    spin_lock(swap_sync_lock);
    glimpl_swap_buffers(real_width, real_height, 1, GL_BGRA);
    XPutImage(dpy, drawable, swap_data.gc, swap_data.ximage, 0, 0, 0, 0, real_width, real_height);
    spin_unlock(swap_sync_lock);

    XSync(dpy, False);
}

void *glXGetProcAddressARB(char *s)
{
    void *proc;
    void *self_handle;

    proc = glx_lookup_builtin_proc(s);
    if (proc != NULL)
        return proc;

    self_handle = glx_get_self_handle();
    if (self_handle != NULL) {
        proc = dlsym(self_handle, s);
        if (proc != NULL)
            return proc;
    }

    return dlsym(RTLD_DEFAULT, s);
}

void *glXGetProcAddress(char *s)
{
    return glXGetProcAddressARB(s);
}

void glximpl_init(void)
{
    char *glx_version_override = getenv("GLX_VERSION_OVERRIDE");

    if (glx_version_override != NULL) {
        glx_majmin_string = glx_version_override;
        glx_major = glx_version_override[0] - '0';
        glx_minor = glx_version_override[2] - '0';
    }

    glx_generate_fb_configs();
}
