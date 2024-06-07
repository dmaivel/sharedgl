#ifndef _SGL_PACKET_H_
#define _SGL_PACKET_H_

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>

/*
 * 256: safe, keeps packet size under 1400 bytes
 * 512: medium
 * 15360: largest, not to be used over networks due to fragmentation
 */
#define SGL_FIFO_UPLOAD_COMMAND_BLOCK_COUNT 512
#define SGL_FIFO_UPLOAD_COMMAND_BLOCK_SIZE (SGL_FIFO_UPLOAD_COMMAND_BLOCK_COUNT * sizeof(uint32_t))

#define SGL_SWAPBUFFERS_RESULT_SIZE 60000

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
};

struct PACKED sgl_packet_swapbuffers_request {
    uint32_t client_id;
    uint32_t width;
    uint32_t height;
    uint32_t vflip;
    uint32_t format;
};

struct PACKED sgl_packet_swapbuffers_result {
    uint32_t client_id;
    uint32_t index;
    uint32_t size;
    uint8_t result[SGL_SWAPBUFFERS_RESULT_SIZE];
};

struct PACKED sgl_packet_fifo_upload {
    uint32_t client_id;
    uint32_t expected_chunks;
    uint32_t index;
    uint32_t count;
    uint32_t commands[SGL_FIFO_UPLOAD_COMMAND_BLOCK_COUNT];
};

struct PACKED sgl_packet_retval {
    union {
        uint32_t retval_split[2];
        uint64_t retval;
    };
    uint32_t retval_v[256 / sizeof(uint32_t)];
};

struct PACKED sgl_packet_sync {
    uint32_t sync;
};
#ifdef _WIN32
__pragma( pack(pop))
#endif

#endif