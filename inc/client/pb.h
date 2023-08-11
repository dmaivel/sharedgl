#ifndef _SGL_PB_H_
#define _SGL_PB_H_

#ifndef _WIN32
#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>
#endif

#include <stdint.h>
#include <inttypes.h>

#ifndef _WIN32
void pb_set(int pb);
#else
void pb_set(void);
void pb_unset(void);
#endif

void pb_reset();
void pb_push(int c);
void pb_pushf(float c);

int pb_read(int s);
long pb_read64(int s);
void pb_write(int s, int c);
void pb_copy(void *data, int s, size_t length);
void pb_memcpy(void *src, size_t length);

void *pb_ptr(size_t offs);

#endif