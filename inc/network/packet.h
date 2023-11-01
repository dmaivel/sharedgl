#ifndef _SGL_PACKET_H_
#define _SGL_PACKET_H_

#include <stdbool.h>

/*
 * 1024 * 63
 */
#define SGL_PACKET_MAX_BLOCK_SIZE 64512

#define SGL_PACKET_MAX_SIZE SGL_PACKET_MAX_BLOCK_SIZE + sizeof(struct sgl_packet_header)

enum {
    /*
     * notify server of our existence
     * server sends this identical packet back (with the same signature) containing the client id
     */
    SGL_PACKET_TYPE_CONNECT,

    /*
     * what gl we runnin?
     */
    SGL_PACKET_TYPE_WHAT_IS_OPENGL,

    /*
     * upload fifo buffer to server
     * server tells client fifo stuff is done
     * 
     * NOTE: RETVAL IS RETURNED TO THE CLIENT
     */
    SGL_PACKET_TYPE_FIFO_UPLOAD,

    /*
     * request framebuffer from server
     *   - slow_but_safe: send small safe packets, min risk
     *   - fast_but_loss: send big packets, risk loss
     */
    SGL_PACKET_TYPE_FRAMEBUFFER_SLOW_BUT_SAFE,
    SGL_PACKET_TYPE_FRAMEBUFFER_FAST_BUT_LOSS,
    SGL_PACKET_TYPE_FRAMEBUFFER_DONE,

    /*
     * request missing data
     */
    SGL_PACKET_TYPE_REQUEST_RECOVERY,
};

struct __attribute__((packed)) sgl_packet_header {
    short client_id;
    bool is_for_server;
    char type;
    unsigned int size;
    short index;
    unsigned short expected_blocks;
    int signature;
};

#endif