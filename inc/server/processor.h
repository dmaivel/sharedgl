#ifndef _SGL_PROCESSOR_H_
#define _SGL_PROCESSOR_H_

#include <stdlib.h>
#include <stdbool.h>

struct sgl_cmd_processor_args {
    /*
     * shared memory information
     */
    void *base_address;
    size_t memory_size;

    /*
     * use network instead of shared memory
     */
    bool network_over_shared;
    int port;

    /*
     * opengl version
     */
    int gl_major;
    int gl_minor;

    /*
     * for debugging; in the event of an exception,
     * this pointer will contain a pointer to the
     * current command in execution
     */
    int **internal_cmd_ptr;
};

void sgl_cmd_processor_start(struct sgl_cmd_processor_args args);

#endif