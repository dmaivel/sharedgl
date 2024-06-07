#ifndef _SGL_GLIMPL_H_
#define _SGL_GLIMPL_H_

#include <commongl.h>

void glimpl_init();
void glimpl_submit();
void glimpl_goodbye();
void glimpl_report(int width, int height);
void glimpl_swap_buffers(int width, int height, int vflip, int format);
void *glimpl_fb_address();

/*
 * gl... functions don't need to be public, however
 * for windows icd we seemingly need to get a proc
 * table for 1.1
 */
#ifdef _WIN32
#include <client/platform/gldrv.h>
PGLCLTPROCTABLE glimpl_GetProcTable();
#endif

#endif