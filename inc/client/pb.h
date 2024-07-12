#ifndef _SGL_PB_H_
#define _SGL_PB_H_

#ifndef _WIN32
#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>
#endif

#include <stdint.h>
#include <stdbool.h>

struct pb_net_hooks {
    int(*_pb_read)(int s);
    int64_t(*_pb_read64)(int s);
    void(*_pb_write)(int s, int c);
    void(*_pb_copy)(void *data, int s, size_t length);
    void(*_pb_memcpy)(void *src, size_t length);
    void*(*_pb_ptr)(size_t offs);
    void(*_pb_copy_to_shared)();
};

void pb_set_net(struct pb_net_hooks hooks, size_t internal_alloc_size);

#ifndef _WIN32
void pb_set(int pb, bool direct_access);
#else
void pb_set(bool direct_access);
void pb_unset(void);
#endif

void pb_reset();
void pb_push(int c);
void pb_pushf(float c);

int pb_read(int s);
int64_t pb_read64(int s);
void pb_write(int s, int c);

void pb_memcpy(const void *src, size_t length);
void pb_memcpy_unaligned(const void *src, size_t length);
void pb_realign();

void *pb_ptr(size_t offs);

/*
 * special case: return pointer within internal space
 * only used for networking junk
 * basically, todo: organize everything !!
 */
void *pb_iptr(size_t offs);

size_t pb_size();

void pb_copy_to_shared();

#endif