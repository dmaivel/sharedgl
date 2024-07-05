#define SHAREDGL_HOST
#include <sharedgl.h>
#include <server/context.h>
#include <server/overlay.h>

static bool is_vid_init = false;
static int mw = 1920;
static int mh = 1080;
static bool is_overlay_string_init = false;

void sgl_set_max_resolution(int width, int height)
{
    mw = width;
    mh = height;
}

void sgl_get_max_resolution(int *width, int *height) 
{
    *width = mw;
    *height = mh;
}

struct sgl_host_context *sgl_context_create()
{
    struct sgl_host_context *context = (struct sgl_host_context *)malloc(sizeof(struct sgl_host_context));

    if (!is_vid_init) {
        SDL_Init(SDL_INIT_VIDEO);
        is_vid_init = true;
    }

    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
    // SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    // SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);

    context->window = SDL_CreateWindow(
        "SDL Offscreen Window",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        mw, mh,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (context->window == NULL) {
        fprintf(stderr, "%s: Failed to create window\n", __func__);
        SDL_Quit();
        exit(1);
    }

    context->gl_context = SDL_GL_CreateContext(context->window);
    if (context->gl_context == NULL) {
        fprintf(stderr, "%s: Failed to create GL context\n", __func__);
        SDL_DestroyWindow(context->window);
        SDL_Quit();
        exit(1);
    }

    sgl_set_current(context);

    if (!is_overlay_string_init) {
        overlay_set_renderer_string((char*)glGetString(GL_RENDERER));
        is_overlay_string_init = true;
    }

    return context;
}

void sgl_context_destroy(struct sgl_host_context *ctx)
{
    sgl_set_current(NULL);
    SDL_DestroyWindow(ctx->window);
    SDL_GL_DeleteContext(ctx->gl_context);
    free(ctx);
}

#ifdef SGL_DEBUG_EMIT_FRAMES
SDL_Window *window;
#endif

void sgl_set_current(struct sgl_host_context *ctx)
{
    if (ctx == NULL)
        SDL_GL_MakeCurrent(NULL, NULL);
    else
        SDL_GL_MakeCurrent(ctx->window, ctx->gl_context);

#ifdef SGL_DEBUG_EMIT_FRAMES
    if (ctx)
        window = ctx->window;
#endif
}

void *sgl_read_pixels(unsigned int width, unsigned int height, void *data, int vflip, int format, size_t mem_usage)
{
    static struct overlay_context overlay_ctx = { 0 };

    overlay_stage1(&overlay_ctx);

    glReadPixels(0, 0, mw, height, format, GL_UNSIGNED_BYTE, data); // GL_BGRA
    int *pdata = data;

    if (vflip) {
        for (int y = 0; y < height / 2; y++) {
            for (int x = 0; x < mw; x++) {
                int *ptop = &pdata[y * mw + x];
                int *pbottom = &pdata[(height - y - 1) * mw + x];

                int vtop = *ptop;
                int vbottom = *pbottom;

                *ptop = vbottom;
                *pbottom = vtop;
            }
        }
    }

    overlay_stage2(&overlay_ctx, data, mw, mem_usage);

#ifdef SGL_DEBUG_EMIT_FRAMES
    SDL_GL_SwapWindow(window);
#endif

    return data;
}