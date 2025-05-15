#ifndef _SGL_PACKET_H_
#define _SGL_PACKET_H_

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>

#ifndef _WIN32
#define PACKED __attribute__((packed))
#else
#define PACKED
#endif

#ifdef _WIN32
__pragma( pack(push, 1) )
#endif
struct PACKED sgl_packet_connect {
    uint32_t client_id;
    uint64_t framebuffer_size;
    uint64_t fifo_size;
    uint32_t gl_major;
    uint32_t gl_minor;
    uint32_t max_width;
    uint32_t max_height;
};

struct PACKED sgl_packet_retval {
    union {
        uint32_t retval_split[2];
        uint64_t retval;
    };
    uint32_t retval_v[256 / sizeof(uint32_t)];
};
#ifdef _WIN32
__pragma( pack(pop))
#endif

#endif
