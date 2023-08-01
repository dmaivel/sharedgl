#ifndef _SGL_GLIMPL_H_
#define _SGL_GLIMPL_H_

#include <commongl.h>

void glimpl_init();
void glimpl_commit();
void glimpl_goodbye();
void glimpl_swap_buffers(int width, int height, int vflip, int format);
void *glimpl_fb_address();

/*
 * gl... functions don't need to be public
 */

#endif