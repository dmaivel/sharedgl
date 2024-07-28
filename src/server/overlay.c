#include <server/overlay.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include <server/overlay_font.h>

static void overlay_draw_char(int *display, int width, char c, int x, int y, unsigned int fg) 
{
    int mask[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    const unsigned char *gylph = IBM + (int)c * CHAR_HEIGHT;

    unsigned char *color;

    for (int cy = 0; cy < CHAR_HEIGHT; cy++)
        for (int cx = 0; cx < CHAR_WIDTH; cx++) {
            /*
             * darken pixels
             */
            color = (unsigned char*)&display[(y + cy) * width + (x + ((CHAR_WIDTH - 1) - cx))];
            color[0] = color[0] * 120 / 255;
            color[1] = color[1] * 120 / 255;
            color[2] = color[2] * 120 / 255;
            color[3] = color[3] * 120 / 255;

            if (gylph[cy] & mask[cx])
                display[(y + cy) * width + (x + ((CHAR_WIDTH - 1) - cx))] = fg;
        }
}

static void overlay_draw_text(int *display, int width, char *text, int x, int y, unsigned int fg) 
{
    char* c = text;
    for (int i = x; *c; i += CHAR_WIDTH)
        overlay_draw_char(
            display,
            width,
            *c++,
            i,
            y,
            fg
        );
}

static bool overlay_enabled = false;
static char overlay_string[256] = "SharedGL using ";

void overlay_set_renderer_string(char *string)
{
    strcpy(overlay_string + 15, string);
}

void overlay_enable()
{
    overlay_enabled = true;
}

void overlay_stage1(struct overlay_context *ctx)
{
    if (!overlay_enabled)
        return;

    ctx->delta_ticks = clock() - ctx->current_ticks;
    if (ctx->current_ticks && ctx->delta_ticks != 0)
        ctx->fps = CLOCKS_PER_SEC / ctx->delta_ticks;
}

void overlay_stage2(struct overlay_context *ctx, int *frame, int width, size_t mem_usage)
{
    if (!overlay_enabled)
        return;

    double usage = mem_usage;
    bool is_mb = false;

    if (mem_usage > 0x100000) {
        usage /= 0x100000;
        is_mb = true;
    }
    else {
        usage /= 0x1000;
    }

    if (ctx->current_ticks) {
        char str[12];
        char mem[24];
        sprintf(str, "FPS: %ld", ctx->fps);
        sprintf(mem, "MEM: %.2f %s", usage, is_mb ? "MB" : "KB");

        overlay_draw_text(frame, width, overlay_string, 0, CHAR_HEIGHT * 0, 0xffffff);
        overlay_draw_text(frame, width, mem, 0, CHAR_HEIGHT * 1, 0xffffff);
        overlay_draw_text(frame, width, str, 0, CHAR_HEIGHT * 2, 0xffffff);
    }

    ctx->current_ticks = clock();
}