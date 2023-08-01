#ifndef _SGL_OVERLAY_H_
#define _SGL_OVERLAY_H_

#include <time.h>

struct overlay_context {
    clock_t current_ticks, delta_ticks;
    clock_t fps;
};

void overlay_enable();
void overlay_stage1(struct overlay_context *ctx);
void overlay_stage2(struct overlay_context *ctx, int *frame, int width);

#endif