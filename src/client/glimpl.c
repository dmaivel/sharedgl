#include <client/glimpl.h>
#include <client/memory.h>
#include <client/spinlock.h>
#include <client/pb.h>
#include <client/scratch.h>

#include <client/platform/icd.h>

#include <sharedgl.h>
#include <commongl.h>

#include <network/net.h>
#include <network/packet.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#ifndef _WIN32
#include <sys/mman.h>
#endif

#define GLIMPL_RUNTIME_USES_SHARED_MEMORY (net_ctx == NULL)

#define GLIMPL_MAX_OBJECTS 256
#define GLIMPL_MAX_TEXTURES 8
#define GLIMPL_MAX_COUNT_FOR_MATRIX_OP 256 // MSVC doesn't support VLAs

// used by glGet*v
#define GL_GET_MEMCPY_RETVAL_EX(name, data, type) \
    switch (name) { \
    case GL_MODELVIEW_MATRIX: \
    case GL_PROJECTION_MATRIX: \
    case GL_TEXTURE_MATRIX: \
        memcpy(data, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(type) * 16); \
        break; \
    case GL_ACCUM_CLEAR_VALUE: \
    case GL_COLOR_CLEAR_VALUE: \
    case GL_COLOR_WRITEMASK: \
    case GL_CURRENT_COLOR: \
    case GL_CURRENT_RASTER_COLOR: \
    case GL_CURRENT_RASTER_POSITION: \
    case GL_CURRENT_RASTER_TEXTURE_COORDS: \
    case GL_CURRENT_TEXTURE_COORDS: \
    case GL_FOG_COLOR: \
    case GL_LIGHT_MODEL_AMBIENT: \
    case GL_MAP2_GRID_DOMAIN: \
    case GL_SCISSOR_BOX: \
    case GL_TEXTURE_ENV_COLOR: \
    case GL_VIEWPORT: \
        memcpy(data, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(type) * 4); \
        break; \
    case GL_CURRENT_NORMAL: \
        memcpy(data, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(type) * 3); \
        break; \
    case GL_DEPTH_RANGE: \
    case GL_LINE_WIDTH_RANGE: \
    case GL_MAP1_GRID_DOMAIN: \
    case GL_MAP2_GRID_SEGMENTS: \
    case GL_MAX_VIEWPORT_DIMS: \
    case GL_POINT_SIZE_RANGE: \
    case GL_POLYGON_MODE: \
    case GL_ALIASED_POINT_SIZE_RANGE: \
        memcpy(data, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(type) * 2); \
        break; \
    default: \
        memcpy(data, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(type) * 1); \
        break; \
    }

#define GL_GET_MEMCPY_RETVAL(data, type) \
    GL_GET_MEMCPY_RETVAL_EX(pname, data, type)

// #define STUB() fprintf(stderr, "%s: stub\n", __FUNCTION__);
#define STUB()

struct gl_vertex_attrib_pointer {
    int index;
    int size;
    int type;
    int normalized;
    int stride;
    int *ptr;

    bool enabled;
    bool client_managed;
};

struct gl_color_tex_vertex_pointer {
    GLint size; 
    GLenum type;
    GLsizei stride;
    const void *pointer;

    bool in_use;
};

struct gl_normal_index_pointer {
    GLenum type;
    GLsizei stride;
    const void *pointer;

    bool in_use;
};

struct gl_edge_flag_pointer {
    GLsizei stride;
    const void *pointer;

    bool in_use;
};

struct gl_map_buffer {
    int target;
    void *mem;
    size_t offset;
    size_t length;
    int access;
    bool in_use;
};

struct gl_vertex_attrib_pointer     glimpl_vaps[GLIMPL_MAX_OBJECTS];

struct gl_color_tex_vertex_pointer  glimpl_color_ptr,
                                    glimpl_tex_coord_ptr[GLIMPL_MAX_TEXTURES],
                                    glimpl_vertex_ptr,
                                    glimpl_color2_ptr;

int                                 glimpl_client_active_texture = 0;

struct gl_normal_index_pointer      glimpl_normal_ptr,
                                    glimpl_index_pointer,
                                    glimpl_interleaved_pointer,
                                    glimpl_fog_coord_pointer;

struct gl_edge_flag_pointer         glimpl_edge_flag_ptr;

struct gl_map_buffer                glimpl_map_buffer;

float                               glimpl_global_matrix_double_to_float[GLIMPL_MAX_COUNT_FOR_MATRIX_OP];

#define NUM_EXTENSIONS 84
static const char *glimpl_extensions_full = "GL_ARB_ES2_compatibility "
                                            "GL_ARB_ES3_compatibility "
                                            "GL_ARB_color_buffer_float "
                                            "GL_ARB_compressed_texture_pixel_storage "
                                            "GL_ARB_compute_shader "
                                            "GL_ARB_conservative_depth "
                                            "GL_ARB_copy_buffer "
                                            "GL_ARB_cull_distance "
                                            "GL_ARB_derivative_control "
                                            "GL_ARB_draw_buffers_blend "
                                            "GL_ARB_draw_elements_base_vertex "
                                            "GL_ARB_draw_indirect "
                                            "GL_ARB_draw_instanced "
                                            "GL_ARB_explicit_uniform_location "
                                            "GL_ARB_fragment_shader "
                                            "GL_ARB_framebuffer_object "
                                            "GL_ARB_geometry_shader4 "
                                            "GL_ARB_get_program_binary "
                                            "GL_ARB_gpu_shader5 "
                                            "GL_ARB_half_float_pixel "
                                            "GL_ARB_internalformat_query "
                                            "GL_ARB_internalformat_query2 "
                                            "GL_ARB_map_buffer_alignment "
                                            "GL_ARB_map_buffer_range "
                                            "GL_ARB_multitexture "
                                            "GL_ARB_program_interface_query "
                                            "GL_ARB_sampler_objects "
                                            "GL_ARB_separate_shader_objects "
                                            "GL_ARB_shader_atomic_counters "
                                            "GL_ARB_shader_bit_encoding "
                                            "GL_ARB_shader_image_load_store "
                                            "GL_ARB_shader_image_size "
                                            "GL_ARB_shader_objects "
                                            "GL_ARB_shader_precision "
                                            "GL_ARB_shading_language_420pack "
                                            "GL_ARB_shading_language_packing "
                                            "GL_ARB_sync "
                                            "GL_ARB_tessellation_shader "
                                            "GL_ARB_texture_buffer_object "
                                            "GL_ARB_texture_buffer_object_rgb32 "
                                            "GL_ARB_texture_buffer_range "
                                            "GL_ARB_texture_compression_bptc "
                                            "GL_ARB_texture_compression_rgtc "
                                            "GL_ARB_texture_cube_map_array "
                                            "GL_ARB_texture_gather "
                                            "GL_ARB_texture_multisample "
                                            "GL_ARB_texture_non_power_of_two "
                                            "GL_ARB_texture_rectangle "
                                            "GL_ARB_texture_rg "
                                            "GL_ARB_texture_storage "
                                            "GL_ARB_texture_swizzle "
                                            "GL_ARB_texture_view "
                                            "GL_ARB_transform_feedback2 "
                                            "GL_ARB_transform_feedback3 "
                                            "GL_ARB_uniform_buffer_object "
                                            "GL_ARB_vertex_array_object "
                                            "GL_ARB_vertex_attrib_binding "
                                            "GL_ARB_vertex_buffer_object "
                                            "GL_ARB_vertex_shader "
                                            "GL_ARB_viewport_array "
                                            "GL_EXT_abgr "
                                            "GL_EXT_bgra "
                                            "GL_EXT_bindable_uniform "
                                            "GL_EXT_draw_buffers2 "
                                            "GL_EXT_framebuffer_blit "
                                            "GL_EXT_framebuffer_multisample "
                                            "GL_EXT_framebuffer_object "
                                            "GL_EXT_framebuffer_sRGB "
                                            "GL_EXT_geometry_shader4 "
                                            "GL_EXT_gpu_shader4 "
                                            "GL_EXT_packed_depth_stencil "
                                            "GL_EXT_polygon_offset_clamp "
                                            "GL_EXT_shader_integer_mix "
                                            "GL_EXT_texture_compression_s3tc "
                                            "GL_EXT_texture_filter_anisotropic "
                                            "GL_EXT_texture_sRGB "
                                            "GL_EXT_texture_sRGB_decode "
                                            "GL_NV_texture_barrier "
                                            "GL_NV_texture_barrier "
                                            "GL_ARB_occlusion_query "
                                            "GL_ARB_vertex_array_bgra "
                                            "GL_EXT_vertex_array_bgra "
                                            "GL_EXT_direct_state_access "
                                            "GL_EXT_framebuffer_multisample_blit_scaled "
                                            "GL_EXT_texture_compression_dxt1";

static const char glimpl_extensions_list[NUM_EXTENSIONS][48] = {
    "GL_ARB_ES2_compatibility",
    "GL_ARB_ES3_compatibility",
    "GL_ARB_color_buffer_float",
    "GL_ARB_compressed_texture_pixel_storage",
    "GL_ARB_compute_shader",
    "GL_ARB_conservative_depth",
    "GL_ARB_copy_buffer",
    "GL_ARB_cull_distance",
    "GL_ARB_derivative_control",
    "GL_ARB_draw_buffers_blend",
    "GL_ARB_draw_elements_base_vertex",
    "GL_ARB_draw_indirect",
    "GL_ARB_draw_instanced",
    "GL_ARB_explicit_uniform_location",
    "GL_ARB_fragment_shader",
    "GL_ARB_framebuffer_object",
    "GL_ARB_geometry_shader4",
    "GL_ARB_get_program_binary",
    "GL_ARB_gpu_shader5",
    "GL_ARB_half_float_pixel",
    "GL_ARB_internalformat_query",
    "GL_ARB_internalformat_query2",
    "GL_ARB_map_buffer_alignment",
    "GL_ARB_map_buffer_range",
    "GL_ARB_multitexture",
    "GL_ARB_program_interface_query",
    "GL_ARB_sampler_objects",
    "GL_ARB_separate_shader_objects",
    "GL_ARB_shader_atomic_counters",
    "GL_ARB_shader_bit_encoding",
    "GL_ARB_shader_image_load_store",
    "GL_ARB_shader_image_size",
    "GL_ARB_shader_objects",
    "GL_ARB_shader_precision",
    "GL_ARB_shading_language_420pack",
    "GL_ARB_shading_language_packing",
    "GL_ARB_sync",
    "GL_ARB_tessellation_shader",
    "GL_ARB_texture_buffer_object",
    "GL_ARB_texture_buffer_object_rgb32",
    "GL_ARB_texture_buffer_range",
    "GL_ARB_texture_compression_bptc",
    "GL_ARB_texture_compression_rgtc",
    "GL_ARB_texture_cube_map_array",
    "GL_ARB_texture_gather",
    "GL_ARB_texture_multisample",
    "GL_ARB_texture_non_power_of_two",
    "GL_ARB_texture_rectangle",
    "GL_ARB_texture_rg",
    "GL_ARB_texture_storage",
    "GL_ARB_texture_swizzle",
    "GL_ARB_texture_view",
    "GL_ARB_transform_feedback2",
    "GL_ARB_transform_feedback3",
    "GL_ARB_uniform_buffer_object",
    "GL_ARB_vertex_array_object",
    "GL_ARB_vertex_attrib_binding",
    "GL_ARB_vertex_buffer_object",
    "GL_ARB_vertex_shader",
    "GL_ARB_viewport_array",
    "GL_EXT_abgr",
    "GL_EXT_bgra",
    "GL_EXT_bindable_uniform",
    "GL_EXT_draw_buffers2",
    "GL_EXT_framebuffer_blit",
    "GL_EXT_framebuffer_multisample",
    "GL_EXT_framebuffer_object",
    "GL_EXT_framebuffer_sRGB",
    "GL_EXT_geometry_shader4",
    "GL_EXT_gpu_shader4",
    "GL_EXT_packed_depth_stencil",
    "GL_EXT_polygon_offset_clamp",
    "GL_EXT_shader_integer_mix",
    "GL_EXT_texture_compression_s3tc",
    "GL_EXT_texture_filter_anisotropic",
    "GL_EXT_texture_sRGB",
    "GL_EXT_texture_sRGB_decode",
    "GL_NV_texture_barrier",
    "GL_ARB_occlusion_query",
    "GL_ARB_vertex_array_bgra",
    "GL_EXT_vertex_array_bgra",
    "GL_EXT_direct_state_access",
    "GL_EXT_framebuffer_multisample_blit_scaled",
    "GL_EXT_texture_compression_dxt1"
};

/* fake variables to be used with network feature only */
static struct net_context *net_ctx = NULL;
static int *fake_register_space = NULL;
static int *fake_framebuffer = NULL;
static int fake_swap_buffers_sync = 0;

static int glimpl_major = SGL_DEFAULT_MAJOR;
static int glimpl_minor = SGL_DEFAULT_MINOR;

static int client_id = 0;
static void *lockg;

/* pb_read hook if using network feature */
static int pb_read_hook(int offset)
{
    switch (offset) {
    case SGL_OFFSET_REGISTER_RETVAL:
        return fake_register_space[0];
    case SGL_OFFSET_REGISTER_GLMAJ:
        return glimpl_major;
    case SGL_OFFSET_REGISTER_GLMIN:
        return glimpl_minor;
    default:
        return fake_register_space[offset];
    }
}

/* pb_read64 hook if using network feature */
static int64_t pb_read64_hook(int offset)
{
    if (offset == SGL_OFFSET_REGISTER_RETVAL)
        return *(int64_t*)&fake_register_space[0];
    return *(int64_t*)&fake_register_space[offset];
}

/* pb_ptr hook if using network feature */
static void *pb_ptr_hook(size_t offset)
{
    switch (offset) {
    case SGL_OFFSET_REGISTER_RETVAL:
        return &fake_register_space[0];
    case SGL_OFFSET_REGISTER_RETVAL_V:
        return &fake_register_space[2];
    case SGL_OFFSET_REGISTER_SWAP_BUFFERS_SYNC:
        return &fake_swap_buffers_sync;
    default:
        fprintf(stderr, "pb_ptr_hook: defaulted to pb_iptr, possible undefined behavior\n");
        return pb_iptr(offset);
    }
}

static void push_string(const char *s)
{
    int len = strlen(s);
    int last = 0;
    int rem = len % 4;
    int shift = 0;

    for (int j = 0; j < len / 4; j++) {
        pb_push(*(int*)&s[j * 4]);
        last = *(int*)&s[j * 4];
    }

    if (rem != 0) {
        int res = 0;
        while (rem) {
            res |= s[len - rem] << shift;
            rem--;
            shift += 8;
        }

        pb_push(res);
        last = res;
    }

    if (!(((last) & 0xFF) == 0 || ((last >> 8) & 0xFF) == 0 || ((last >> 16) & 0xFF) == 0 || ((last >> 24) & 0xFF) == 0))
        pb_push(0);
}

/*
 * temporary solution to submit stalling out at `recv` when
 * submitting the goodbye message
 */
bool expecting_retval = true;

static inline void submit_shm()
{
    /*
     * lock
     */
    spin_lock(lockg);

    /* 
     * hint to server that we're ready 
     */
    pb_write(SGL_OFFSET_REGISTER_READY_HINT, client_id);

    /*
     * copy internal buffer to shared memory and submit
     */
    pb_copy_to_shared();
    pb_write(SGL_OFFSET_REGISTER_SUBMIT, 1);
    while (pb_read(SGL_OFFSET_REGISTER_SUBMIT) == 1);
    pb_reset();

    /*
     * unlock
     */
    spin_unlock(lockg);
}

static inline void submit_net()
{
    void *ptr = pb_iptr(0);
    size_t size = pb_size();
    int blocks = CEIL_DIV(size, SGL_FIFO_UPLOAD_COMMAND_BLOCK_SIZE);
    size_t min_size = 0;
    size_t count = size / 4;
    struct sgl_packet_retval packet;

    /*
     * packet
     */
    for (int i = 0; i < blocks; i++) {
        size_t packet_count = MIN(count, SGL_FIFO_UPLOAD_COMMAND_BLOCK_COUNT);

        struct sgl_packet_fifo_upload packet = {
            /* client_id = */       client_id,
            /* expected_chunks = */ blocks,
            /* index = */           i,
            /* count = */           packet_count,
            /* commands = */        { 0 }
        };

        memcpy(packet.commands, (char*)ptr + (i * SGL_FIFO_UPLOAD_COMMAND_BLOCK_SIZE), packet_count * sizeof(uint32_t));
        net_send_tcp(net_ctx, NET_SOCKET_SERVER, &packet, sizeof(packet));

        count -= SGL_FIFO_UPLOAD_COMMAND_BLOCK_COUNT;
    }

    /*
     * get retval registers
     */
    if (expecting_retval)
        net_recv_tcp_timeout(net_ctx, NET_SOCKET_SERVER, &packet, sizeof(packet), 500);

    /*
     * write response into fake register space
     */
    memcpy(fake_register_space, &packet, 256 + 8); 
    pb_reset();
}

void glimpl_submit()
{
    /*
     * processor stops at 0
     */
    pb_push(0);

    if (GLIMPL_RUNTIME_USES_SHARED_MEMORY)
        submit_shm();
    else
        submit_net();
}

void glimpl_goodbye()
{
    expecting_retval = false;

    /*
     * probably not a good idea to submit
     * all commands before sending our
     * goodbye to the server, so instead
     * we should just overwrite the beginning
     * of the push buffer in case there are
     * any commands leftover
     */
    pb_reset();
    pb_push(SGL_CMD_GOODBYE_WORLD);
    pb_push(client_id);
    pb_push(0);
    glimpl_submit();

    // if (net_ctx != NULL)
    //     net_goodbye(net_ctx);

#ifdef _WIN32
    pb_unset();
#endif
}

static inline void swap_buffers_shm(int width, int height, int vflip, int format)
{
    pb_push(SGL_CMD_SWAP_BUFFERS);
    pb_push(width);
    pb_push(height);
    pb_push(vflip);
    pb_push(format);
    glimpl_submit();
}

static inline void swap_buffers_net(int width, int height, int vflip, int format)
{
    int expected = CEIL_DIV((width * height * 4), SGL_SWAPBUFFERS_RESULT_SIZE);
    struct sgl_packet_sync sync;

    glimpl_submit();

    struct sgl_packet_swapbuffers_request packet = {
        /* client_id = */   client_id,
        /* width = */       width,
        /* height = */      height,
        /* vflip = */       vflip,
        /* format = */      format
    };

    net_send_udp(net_ctx, &packet, sizeof(packet), 0);

    // wait for sync, timeout appears to disturb fifo upload
    net_recv_tcp(net_ctx, NET_SOCKET_SERVER, &sync, sizeof(sync));

    for (int i = 0; i < expected * 4; i++) {
        struct sgl_packet_swapbuffers_result result;
        // while (net_recv_udp(net_ctx, &result, sizeof(result), 0) == -1);
        int recieved = net_recv_udp(net_ctx, &result, sizeof(result), 0);

        // global, so throwout any results that aren't ours (possible vuln for other applications? idk)
        if (result.client_id != client_id || recieved < 0) {
            // i--;
            continue;
        }

        memcpy((char*)fake_framebuffer + (result.index * SGL_SWAPBUFFERS_RESULT_SIZE), result.result, result.size);
    }
}

void glimpl_swap_buffers(int width, int height, int vflip, int format)
{
    if (GLIMPL_RUNTIME_USES_SHARED_MEMORY)
        swap_buffers_shm(width, height, vflip, format);
    else
        swap_buffers_net(width, height, vflip, format);
}

void *glimpl_fb_address()
{
    /* fake framebuffer used with network feature only */
    if (!GLIMPL_RUNTIME_USES_SHARED_MEMORY)
        return fake_framebuffer;
    return pb_ptr(pb_read64(SGL_OFFSET_REGISTER_FBSTART));
}

static inline void init_shm(bool use_direct_access)
{
#ifndef _WIN32
    int fd = shm_open(SGL_SHARED_MEMORY_NAME, O_RDWR, S_IRWXU);
    if (fd == -1)
        fd = sgl_detect_device_memory("/dev/sharedgl");
    if (fd == -1) {
        fprintf(stderr, "init_shm: failed to find memory\n");
        exit(1);
    }

    pb_set(fd, use_direct_access);
#else
    pb_set(use_direct_access);
#endif
    pb_reset();
}

static inline void shm_create_context()
{
    lockg = pb_ptr(SGL_OFFSET_REGISTER_LOCK);

    /*
     * claim client id and increment the register for the
     * next claimee to claim
     */
    spin_lock(lockg);
    client_id = pb_read(SGL_OFFSET_REGISTER_CLAIM_ID);
    pb_write(SGL_OFFSET_REGISTER_READY_HINT, client_id);
    pb_write(SGL_OFFSET_REGISTER_CLAIM_ID, client_id + 1);

    /*
     * notify the server we would like to connect
     */
    pb_write(SGL_OFFSET_REGISTER_CONNECT, client_id);
    spin_unlock(lockg);
        
    /*
     * submit
     */
    pb_push(SGL_CMD_CREATE_CONTEXT);
    glimpl_submit();
    
    int packed_dims = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    icd_set_max_dimensions(UNPACK_A(packed_dims), UNPACK_B(packed_dims));
}

static inline void init_net(char *network)
{
    struct sgl_packet_connect packet = { 0 };
    char *res;
    struct pb_net_hooks hooks = {
        pb_read_hook,
        pb_read64_hook,
        NULL,
        NULL,
        NULL,
        pb_ptr_hook,
        NULL
    };

    /*
     * string is formatted address:port, so we want to split it into two strings
     */
    for (char *c = network; *c; c++) {
        if (*c == ':') {
            *c = 0;
            break;
        }
    }

    res = net_init_client(&net_ctx, network, atoi(&network[strlen(network) + 1]));
    if (res != NULL) {
        fprintf(stderr, "init_net: could not initialize client (%s)\n", res);
        exit(1);
    }

    net_recv_tcp(net_ctx, NET_SOCKET_SERVER, &packet, sizeof(packet));

    glimpl_major = packet.gl_major;
    glimpl_minor = packet.gl_minor;

    client_id = packet.client_id;

    fake_register_space = malloc(SGL_OFFSET_COMMAND_START);
    fake_framebuffer = malloc(packet.framebuffer_size);

    pb_set_net(hooks, packet.fifo_size);

    icd_set_max_dimensions(packet.max_width, packet.max_height);
}

void glimpl_init()
{
    char *network = getenv("SGL_NET_OVER_SHARED");
    char *gl_version_override = getenv("GL_VERSION_OVERRIDE");

    if (network == NULL)
        init_shm(false);
    else
        init_net(network);

    glimpl_major = gl_version_override ? gl_version_override[0] - '0' : pb_read(SGL_OFFSET_REGISTER_GLMAJ);
    glimpl_minor = gl_version_override ? gl_version_override[2] - '0' : pb_read(SGL_OFFSET_REGISTER_GLMIN);

    if (GLIMPL_RUNTIME_USES_SHARED_MEMORY)
        shm_create_context();

    glimpl_map_buffer.mem = scratch_buffer_get(0x1000);
}

static struct gl_vertex_attrib_pointer *glimpl_get_enabled_vap()
{
    for (int i = 0; i < GLIMPL_MAX_OBJECTS; i++)
        if (glimpl_vaps[i].enabled)
            return &glimpl_vaps[i];

    return NULL;
}

static inline size_t glimpl_get_pixel_size(GLenum format)
{
    switch (format) {
    case GL_RGBA:
    case GL_RGBA8:
    case GL_BGRA:
    case GL_COMPRESSED_SRGB_ALPHA:
        return 4;
    case GL_RGB:
    case GL_BGR:
    case GL_RGB8:
    case GL_DEPTH_COMPONENT:
        return 3;
    case GL_LUMINANCE_ALPHA:
    case GL_RG:
    case GL_RG8:
        return 2;
    case GL_LUMINANCE:
    case GL_ALPHA:
    case GL_RED:
    case GL_R8:
        return 1;
    case GL_RGB16F:
    case GL_RGB16I:
    case GL_RGB16UI:
        return 6;
    case GL_RGBA16F:
    case GL_RGBA16I:
    case GL_RGBA16UI:
        return 8;
    default:
        STUB();
        return 1;
    }
}

static inline size_t glimpl_type_size(GLenum type)
{
    switch(type) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:
        return sizeof(GLbyte);
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
        return sizeof(GLshort);
    case GL_INT_2_10_10_10_REV:
    case GL_INT:
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT:
        return sizeof(GLint);
    case GL_FLOAT:
        return sizeof(GLfloat);
    case GL_DOUBLE:
        return sizeof(GLdouble);
    case GL_FIXED:
        return sizeof(GLfixed);
    case GL_HALF_FLOAT:
        return sizeof(GLshort);
    }

    STUB();
    return 1;
}

static inline void glimpl_upload_buffer(const void *data, size_t size)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(CEIL_DIV(size, 4));
    pb_memcpy((void*)data, size);
}

static inline void glimpl_download_buffer(void *dst, size_t size)
{
    int blocks = CEIL_DIV(size, SGL_VP_DOWNLOAD_BLOCK_SIZE_IN_BYTES);
    size_t count = size;

    void *retval_v = pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V);

    for (int i = 0; i < blocks; i++) {
        size_t true_count = MIN(count, SGL_VP_DOWNLOAD_BLOCK_SIZE_IN_BYTES);

        pb_push(SGL_CMD_VP_DOWNLOAD);
        pb_push(true_count);
        
        glimpl_submit();

        memcpy((char*)dst + (i * SGL_VP_DOWNLOAD_BLOCK_SIZE_IN_BYTES), retval_v, true_count);

        count -= SGL_VP_DOWNLOAD_BLOCK_SIZE_IN_BYTES;
    }
}

static void glimpl_upload_texture(GLsizei width, GLsizei height, GLsizei depth, GLenum format, const void* pixels)
{
    if (pixels == NULL) {
        pb_push(SGL_CMD_VP_NULL);
        return;
    }

    size_t total_size = width * height * depth * glimpl_get_pixel_size(format);

    glimpl_upload_buffer(pixels, total_size);
}

static inline void glimpl_push_vertex_attrib_pointers(int count)
{
    for (int i = 0; i < GLIMPL_MAX_OBJECTS; i++) {
        struct gl_vertex_attrib_pointer *vap = &glimpl_vaps[i];
        if (vap->client_managed) {
            glimpl_upload_buffer(vap->ptr, vap->size * count * glimpl_type_size(vap->type));

            pb_push(SGL_CMD_VERTEXATTRIBPOINTER);
            pb_push(vap->index);
            pb_push(vap->size);
            pb_push(vap->type);
            pb_push(vap->normalized);
            pb_push(vap->stride);
            pb_push(LIKELY_OFFSET_LIMIT + 1); // force server to use upload
        }
    }
}

static inline bool glimpl_push_client_pointer(int count, int size, int type, int stride, const void *pointer)
{
    const unsigned char *data = pointer;
    size_t sizeof_type = glimpl_type_size(type);

    if (is_value_likely_an_offset(pointer))
        return false;

    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push((count * size) / CEIL_DIV(sizeof(int), sizeof_type));

    if (stride == 0)
        pb_memcpy(data, count * size * sizeof_type);
    else {
        for (int i = 0; i < count; i++) {
            void *real_data = ((char*)data + i * stride);
            pb_memcpy_unaligned(real_data, size * sizeof_type);
        }
        pb_realign();
    }

    return true;
}

static inline void glimpl_push_client_pointers(int mode, int count)
{
    if (glimpl_normal_ptr.in_use) {
        bool status = glimpl_push_client_pointer(count, 3, /* probably not glimpl_vertex_ptr.size */
                            glimpl_normal_ptr.type, glimpl_normal_ptr.stride, glimpl_normal_ptr.pointer);

        pb_push(SGL_CMD_NORMALPOINTER);
        pb_push(glimpl_normal_ptr.type);
        pb_push(status ? 0 : glimpl_normal_ptr.stride);
        pb_push(status);
        pb_push(status ? 0 : (int)(uintptr_t)glimpl_normal_ptr.pointer);
    }

    if (glimpl_color_ptr.in_use) {
        int true_size = glimpl_color_ptr.size;
        if (true_size > 8)
            true_size = glimpl_get_pixel_size(true_size);

        bool status = glimpl_push_client_pointer(count, true_size, 
                            glimpl_color_ptr.type, glimpl_color_ptr.stride, glimpl_color_ptr.pointer);

        pb_push(SGL_CMD_COLORPOINTER);
        pb_push(glimpl_color_ptr.size);
        pb_push(glimpl_color_ptr.type);
        pb_push(status ? 0 : glimpl_color_ptr.stride);
        pb_push(status);
        pb_push(status ? 0 : (int)(uintptr_t)glimpl_color_ptr.pointer);
    }

    for (int t = 0; t < GLIMPL_MAX_TEXTURES; t++) {
        if (glimpl_tex_coord_ptr[t].in_use) {
            bool status = glimpl_push_client_pointer(count, glimpl_tex_coord_ptr[t].size, 
                                glimpl_tex_coord_ptr[t].type, glimpl_tex_coord_ptr[t].stride, glimpl_tex_coord_ptr[t].pointer);

            pb_push(SGL_CMD_CLIENTACTIVETEXTURE);
            pb_push(GL_TEXTURE0 + t);

            pb_push(SGL_CMD_ENABLECLIENTSTATE);
            pb_push(GL_TEXTURE_COORD_ARRAY);

            pb_push(SGL_CMD_TEXCOORDPOINTER);
            pb_push(glimpl_tex_coord_ptr[t].size);
            pb_push(glimpl_tex_coord_ptr[t].type);
            pb_push(status ? 0 : glimpl_tex_coord_ptr[t].stride);
            pb_push(status);
            pb_push(status ? 0 : (int)(uintptr_t)glimpl_tex_coord_ptr[t].pointer);
        }
    }

    if (glimpl_vertex_ptr.in_use) {
        bool status = glimpl_push_client_pointer(count, glimpl_vertex_ptr.size,
                            glimpl_vertex_ptr.type,  glimpl_vertex_ptr.stride, glimpl_vertex_ptr.pointer);

        pb_push(SGL_CMD_VERTEXPOINTER);
        pb_push(glimpl_vertex_ptr.size);
        pb_push(glimpl_vertex_ptr.type);
        pb_push(status ? 0 : glimpl_vertex_ptr.stride);
        pb_push(status);
        pb_push(status ? 0 : (int)(uintptr_t)glimpl_vertex_ptr.pointer);
    }
}

#define GET_MAX_INDEX(x, y, s, e) \
    for (int i = s; i < e; i++) \
        if (y[i] > x) \
            x = y[i]

static inline void glimpl_draw_elements(int mode, int type, int start, int end, const void *indices)
{
    if (indices) {
        int max_index = 0;
        switch (type) {
        case GL_UNSIGNED_BYTE: {
            const unsigned char *tindices = indices;
            GET_MAX_INDEX(max_index, tindices, start, end);
            break;
        }
        case GL_UNSIGNED_SHORT: {
            const unsigned short *tindices = indices;
            GET_MAX_INDEX(max_index, tindices, start, end);
            break;
        }
        case GL_UNSIGNED_INT: {
            const unsigned int *tindices = indices;
            GET_MAX_INDEX(max_index, tindices, start, end);
            break;
        }
        }

        // increment bc we want count, not the value of the largest index
        max_index++;
        glimpl_push_vertex_attrib_pointers(max_index);
        glimpl_push_client_pointers(mode, max_index);

        /*
         * to-do: pack?
         */
        pb_push(SGL_CMD_VP_UPLOAD);
        pb_push(end - start);
        switch (type) {
        case GL_UNSIGNED_BYTE: {
            const unsigned char *b = indices;
            for (int i = start; i < end; i++)
                pb_push(*b++);
            break;
        }
        case GL_UNSIGNED_SHORT: {
            const unsigned short *s = indices;
            for (int i = start; i < end; i++)
                pb_push(*s++);
            break;
        }
        case GL_UNSIGNED_INT: {
            const unsigned int *u = indices;
            for (int i = start; i < end; i++)
                pb_push(*u++);
            break;
        }
        }
    }
    
    pb_push(SGL_CMD_DRAWELEMENTS);
    pb_push(mode);
    pb_push(end - start);
    pb_push(indices != NULL ? GL_UNSIGNED_INT : type); /* to-do: actually use type */
    pb_push(indices != NULL);
}

#undef GET_MAX_INDEX

static void glimpl_texture_image(int cmd, int n_dims, GLuint texture, GLint level, GLint internalformat,
        GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels)
{
    const int dims[3] = { width, height, depth };

    glimpl_upload_texture(width, n_dims > 1 ? height : 1, n_dims > 2 ? depth : 1, format, pixels);

    pb_push(cmd);
    pb_push(texture);
    pb_push(level);
    pb_push(internalformat);
    for (int i = 0; i < n_dims; i++)
        pb_push(dims[i]);
    pb_push(border);
    pb_push(format);
    pb_push(type);
}

static void glimpl_texture_subimage(int cmd, int n_dims, GLuint texture, GLint level, GLint xoffset, GLint yoffset, 
        GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void *pixels)
{
    const int offsets[3] = { xoffset, yoffset, zoffset };
    const int dims[3] = { width, height, depth };

    glimpl_upload_texture(width, n_dims > 1 ? height : 1, n_dims > 2 ? depth : 1, format, pixels);

    pb_push(cmd);
    pb_push(texture);
    pb_push(level);
    for (int i = 0; i < n_dims; i++)
        pb_push(offsets[i]);
    for (int i = 0; i < n_dims; i++)
        pb_push(dims[i]);
    pb_push(format);
    pb_push(type);
}

static void glimpl_compressed_texture_subimage(int cmd, int n_dims, GLuint texture, GLint level, GLint xoffset, GLint yoffset, 
        GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void *data)
{
    const int offsets[3] = { xoffset, yoffset, zoffset };
    const int dims[3] = { width, height, depth };

    glimpl_upload_buffer((void*)data, imageSize);

    pb_push(cmd);
    pb_push(texture);
    pb_push(level);
    for (int i = 0; i < n_dims; i++)
        pb_push(offsets[i]);
    for (int i = 0; i < n_dims; i++)
        pb_push(dims[i]);
    pb_push(format);
    pb_push(imageSize);
}

static void glimpl_buffer_store_data(int cmd, GLuint buffer, GLsizeiptr size, const void *data, GLenum usage)
{
    if (data != NULL)
        glimpl_upload_buffer(data, size);
    
    pb_push(cmd);
    pb_push(buffer);
    pb_push(size);
    pb_push(data != NULL);
    pb_push(usage);
}

static void glimpl_buffer_subdata(int cmd, GLuint buffer, GLintptr offset, GLsizeiptr size, const void *data)
{
    glimpl_upload_buffer(data, size);
    
    pb_push(cmd);
    pb_push(buffer);
    pb_push(offset);
    pb_push(size);
}

static void glimpl_buffer_clear_data(int cmd, bool is_subdata, GLenum buffer, GLenum internalformat, GLintptr offset, 
        GLsizeiptr size, GLenum format, GLenum type, const void* data)
{
    pb_push(cmd);
    pb_push(buffer);
    pb_push(internalformat);

    if (is_subdata) {
        pb_push(offset);
        pb_push(size);
    }

    pb_push(format);
    pb_push(type);
    pb_push(*(unsigned int*)data); // to-do: technically should look at type first, dont care
}

static void glimpl_get_buffer_parameter(int cmd, GLuint buffer, GLenum pname, GLint* params)
{
    pb_push(cmd);
    pb_push(buffer);
    pb_push(pname);

    glimpl_submit();
    *params = pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

static void *glimpl_map_buffer_range(int cmd, GLenum buffer, GLintptr offset, GLsizeiptr length, GLbitfield access)
{
    bool ranged = true;
    // if (glimpl_map_buffer.in_use) {
    //     fprintf(stderr, "glimpl_map_buffer_range: map buffer already in use, returning NULL\n");
    //     return glimpl_map_buffer.mem;
    // }

    /*
     * length is 0 when this function is called by glMapBuffer, glMapNamedBuffer
     */
    if (length == 0) {
        bool is_named = cmd == SGL_CMD_MAPNAMEDBUFFER;
        int get_param_cmd = !is_named ? SGL_CMD_GETBUFFERPARAMETERIV : SGL_CMD_GETNAMEDBUFFERPARAMETERIV;
        glimpl_get_buffer_parameter(get_param_cmd, buffer, GL_BUFFER_SIZE, (GLint*)&length);
        ranged = false;
    }

    glimpl_map_buffer = (struct gl_map_buffer){
        /* target = */  buffer,
        /* mem = */     scratch_buffer_get(length), // calloc(length, 1), // glimpl_map_buffer.mem,
        /* offset = */  offset,
        /* length = */  length,
        /* access = */  access,
        /* in_use = */  true
    };

    /*
     * this function supports both glMapBuffer and glMapBufferRange; as such, these functions have
     * different parameters to be pushed
     */
    pb_push(cmd);
    if (ranged) {
        pb_push(buffer);
        pb_push(offset);
        pb_push(length);
        pb_push(access);
    }
    else {
        pb_push(buffer);
        pb_push(access);
    }

    glimpl_submit();

    glimpl_download_buffer(glimpl_map_buffer.mem, length);

    return glimpl_map_buffer.mem;
}

static inline bool glimpl_unmap_buffer(int cmd, int buffer)
{
    if (glimpl_map_buffer.target != buffer) {
        fprintf(stderr, "glimpl_unmap_buffer: target mismatch\n");
        return GL_FALSE;
    }

    glimpl_upload_buffer(glimpl_map_buffer.mem, glimpl_map_buffer.length);

    pb_push(cmd);
    pb_push(buffer);
    pb_push(glimpl_map_buffer.length); // internal

    glimpl_submit();
    int res = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    return *(bool*)&res;
}

static inline void glimpl_flush_mapped_buffer_range(int cmd, int buffer, int offset, int length)
{
    glimpl_upload_buffer((char*)glimpl_map_buffer.mem + offset, length);
    
    pb_push(cmd);
    pb_push(buffer);
    pb_push(offset);
    pb_push(length);
}

static void glimpl_invalidate_framebuffer(int cmd, bool is_subframebuffer, GLenum framebuffer, GLsizei n_attachments, 
        const GLenum *attachments, int x, int y, int width, int height)
{
    glimpl_upload_buffer(attachments, n_attachments * sizeof(*attachments));
    pb_push(cmd);
    pb_push(framebuffer);
    pb_push(n_attachments);

    if (is_subframebuffer) {
        pb_push(x);
        pb_push(y);
        pb_push(width);
        pb_push(height);
    }
}

#define PUSH_ALL_VALUES(push, v) \
    push(v[0]); \
    push(buffer == GL_COLOR ? v[1] : 0); \
    push(buffer == GL_COLOR ? v[2] : 0); \
    push(buffer == GL_COLOR ? v[3] : 0);

static void glimpl_clear_buffer(int cmd, bool is_framebuffer, GLenum type, GLuint framebuffer, GLenum buffer, GLint drawbuffer, void *value)
{
    pb_push(cmd);
    if (is_framebuffer)
        pb_push(framebuffer);
    pb_push(buffer);
    pb_push(drawbuffer);

    switch (type) {
    case GL_INT: {
        int *v = value;
        PUSH_ALL_VALUES(pb_push, v);
        break;
    }
    case GL_UNSIGNED_INT: {
        unsigned int *v = value;
        PUSH_ALL_VALUES(pb_push, v);
        break;
    }
    case GL_FLOAT: {
        float *v = value;
        PUSH_ALL_VALUES(pb_pushf, v);
        break;
    }
    }
}

#undef PUSH_ALL_VALUES

static void glimpl_get_framebuffer_parameter(int cmd, bool is_attachment, GLenum framebuffer, GLenum attachment, GLenum pname, GLint* params)
{
    pb_push(cmd);
    pb_push(framebuffer);
    if (is_attachment)
        pb_push(attachment);
    pb_push(pname);

    glimpl_submit();
    *params = pb_read(SGL_OFFSET_REGISTER_RETVAL_V);
}

static void glimpl_get_renderbuffer_parameter(int cmd, GLuint renderbuffer, GLenum pname, GLint* params)
{
    pb_push(cmd);
    pb_push(renderbuffer);
    pb_push(pname);

    glimpl_submit();
    *params = pb_read(SGL_OFFSET_REGISTER_RETVAL_V);
}

static void glimpl_texture_parameter_v(int cmd, GLenum texture, GLenum pname, const void *params)
{
    pb_push(cmd);
    pb_push(texture);
    pb_push(pname);
    if (pname == GL_TEXTURE_BORDER_COLOR || pname == GL_TEXTURE_SWIZZLE_RGBA)
        pb_memcpy(params, 4 * sizeof(int));
    else {
        pb_memcpy(params, 1 * sizeof(int));
        pb_push(0);
        pb_push(0);
        pb_push(0);
    }
}

static void glimpl_get_texture_level_parameter(int cmd, GLenum texture, GLint level, GLenum pname, void *params)
{
    pb_push(cmd);
    pb_push(texture);
    pb_push(level);
    pb_push(pname);
    
    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), 4);
}

static void glimpl_get_texture_parameter(int cmd, GLenum texture, GLenum pname, void *params)
{
    pb_push(cmd);
    pb_push(texture);
    pb_push(pname);

    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(float) + ((pname == GL_TEXTURE_BORDER_COLOR || pname == GL_TEXTURE_SWIZZLE_RGBA) * (3 * sizeof(float))));
}

/* start of real gl functions */

void glAttachShader(GLuint program, GLuint shader)
{
    pb_push(SGL_CMD_ATTACHSHADER);
    pb_push(program);
    pb_push(shader);
}

void glBegin(GLenum mode) 
{
    pb_push(SGL_CMD_BEGIN);
    pb_push(mode);
}

void glBeginQuery(GLenum target, GLuint id)
{
    pb_push(SGL_CMD_BEGINQUERY);
    pb_push(target);
    pb_push(id);
}

void glBindBuffer(GLenum target, GLuint buffer)
{
    pb_push(SGL_CMD_BINDBUFFER);
    pb_push(target);
    pb_push(buffer);
}

void glBindBuffersBase(GLenum target, GLuint first, GLsizei count, const GLuint *buffers)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count); /* could be very bad mistake */
    for (int i = 0; i < count; i++)
        pb_push(buffers[i]);
    
    pb_push(SGL_CMD_BINDBUFFERSBASE);
    pb_push(target);
    pb_push(first);
    pb_push(count);
}

void glBindFragDataLocation(GLuint program, GLuint color, const GLchar* name)
{
    pb_push(SGL_CMD_BINDFRAGDATALOCATION);
    pb_push(program);
    pb_push(color);
    push_string(name);
}

void glBindVertexArray(GLuint array)
{
    pb_push(SGL_CMD_BINDVERTEXARRAY);
    pb_push(array);
}

void glBitmap(GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig, GLfloat xmove, GLfloat ymove, const GLubyte* bitmap)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(width * height / 4); /* could be very bad mistake */
    for (int i = 0; i < (width * height / 4); i++)
        pb_push(bitmap[i * 4] | bitmap[i * 4 + 1] << 8 | bitmap[i * 4 + 2] << 16 | bitmap[i * 4 + 3] << 24);

    pb_push(SGL_CMD_BITMAP);
    pb_push(width);
    pb_push(height);
    pb_push(xorig);
    pb_push(yorig);
    pb_push(xmove);
    pb_push(ymove);
}

void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
    pb_push(SGL_CMD_BLENDFUNC);
    pb_push(sfactor);
    pb_push(dfactor);
}

void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    glimpl_buffer_store_data(SGL_CMD_BUFFERDATA, target, size, data, usage);
}

void glCallList(GLuint list)
{
    pb_push(SGL_CMD_CALLLIST);
    pb_push(list);
}

void glClear(GLbitfield mask)
{
    pb_push(SGL_CMD_CLEAR);
    pb_push(mask);
}

void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha)
{
    pb_push(SGL_CMD_CLEARCOLOR);
    pb_pushf(red);
    pb_pushf(green);
    pb_pushf(blue);
    pb_pushf(alpha);
}

void glClipPlane(GLenum plane, const GLdouble* equation)
{
    pb_push(SGL_CMD_CLIPPLANE);
    pb_push(plane);
    pb_pushf(equation[0]);
    pb_pushf(equation[1]);
    pb_pushf(equation[2]);
    pb_pushf(equation[3]);
}

void glClipPlanef(GLenum p, const GLfloat* eqn)
{
    pb_push(SGL_CMD_CLIPPLANE);
    pb_push(p);
    pb_pushf(eqn[0]);
    pb_pushf(eqn[1]);
    pb_pushf(eqn[2]);
    pb_pushf(eqn[3]);
}

void glColor3f(GLfloat red, GLfloat green, GLfloat blue) 
{
    pb_push(SGL_CMD_COLOR3F);
    pb_pushf(red);
    pb_pushf(green);
    pb_pushf(blue);
}

void glCompileShader(GLuint shader)
{
    pb_push(SGL_CMD_COMPILESHADER);
    pb_push(shader);
}

GLuint glCreateProgram()
{
    pb_push(SGL_CMD_CREATEPROGRAM);
    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

GLuint glCreateShader(GLenum type)
{
    pb_push(SGL_CMD_CREATESHADER);
    pb_push(type);
    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glDeleteBuffers(GLsizei n, const GLuint* buffers)
{
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_DELETEBUFFERS);
        pb_push(buffers[i]);
    }
}

void glDeleteTextures(GLsizei n, const GLuint* textures)
{
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_DELETETEXTURES);
        pb_push(textures[i]);
    }
}

void glDeleteVertexArrays(GLsizei n, const GLuint* arrays)
{
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_DELETEVERTEXARRAYS);
        pb_push(arrays[i]);
    }
}

void glDepthFunc(GLenum func) 
{
    pb_push(SGL_CMD_DEPTHFUNC);
    pb_push(func);
}

void glDeleteProgram(GLuint program)
{
    pb_push(SGL_CMD_DELETEPROGRAM);
    pb_push(program);
}

void glDeleteShader(GLuint shader)
{
    pb_push(SGL_CMD_DELETESHADER);
    pb_push(shader);
}

void glDetachShader(GLuint program, GLuint shader)
{
    pb_push(SGL_CMD_DETACHSHADER);
    pb_push(program);
    pb_push(shader);
}

void glDisable(GLenum cap)
{
    pb_push(SGL_CMD_DISABLE);
    pb_push(cap);
}

void glDispatchCompute(GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z)
{
    pb_push(SGL_CMD_DISPATCHCOMPUTE);
    pb_push(num_groups_x);
    pb_push(num_groups_y);
    pb_push(num_groups_z);
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    glimpl_push_vertex_attrib_pointers(count);
    glimpl_push_client_pointers(mode, count);

    pb_push(SGL_CMD_DRAWARRAYS);
    pb_push(mode);
    pb_push(first);
    pb_push(count);

    // vap->enabled = 0;
}

void glDrawBuffer(GLenum buf)
{
    pb_push(SGL_CMD_DRAWBUFFER);
    pb_push(buf);
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
{
    /*
     * to-do: check if mode and type is valid
     */

    glimpl_draw_elements(mode, type, 0, count, indices);
}

void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices)
{
    /*
     * count is ignored?
     */

    glimpl_draw_elements(mode, type, start, end, indices);
}

void glEnable(GLenum cap)
{
    pb_push(SGL_CMD_ENABLE);
    pb_push(cap);
}

void glEnableVertexAttribArray(GLuint index)
{
    glimpl_vaps[index].enabled = true;

    // if (!glimpl_vaps[index].client_managed) {
        pb_push(SGL_CMD_ENABLEVERTEXATTRIBARRAY);
        pb_push(index);
    // }
}

void glEnd(void) 
{
    pb_push(SGL_CMD_END);
}

void glEndList(void)
{
    pb_push(SGL_CMD_ENDLIST);
}

void glEndQuery(GLenum target)
{
    pb_push(SGL_CMD_ENDQUERY);
    pb_push(target);   
}

void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar)
{
    pb_push(SGL_CMD_FRUSTUM);
    pb_pushf(left);
    pb_pushf(right);
    pb_pushf(bottom);
    pb_pushf(top);
    pb_pushf(zNear);
    pb_pushf(zFar);
}

void glGenBuffers(GLsizei n, GLuint* buffers)
{
    GLuint *p = buffers;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_GENBUFFERS);
        pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGenFramebuffers(GLsizei n, GLuint* framebuffers)
{
    GLuint *p = framebuffers;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_GENFRAMEBUFFERS);
        pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

GLuint glGenLists(GLsizei range)
{
    pb_push(SGL_CMD_GENLISTS);
    pb_push(range);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glGenQueries(GLsizei n, GLuint* ids)
{
    GLuint *p = ids;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_GENQUERIES);
        pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGenTextures(GLsizei n, GLuint* textures)
{
    GLuint *p = textures;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_GENTEXTURES);
        pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGenVertexArrays(GLsizei n, GLuint* arrays)
{
    GLuint *p = arrays;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_GENVERTEXARRAYS);
        pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64 *params)
{
    pb_push(SGL_CMD_GETQUERYOBJECTUI64V);
    pb_push(id);
    pb_push(pname);

    glimpl_submit();
    *params = pb_read64(SGL_OFFSET_REGISTER_RETVAL);
}

void glGetProgramiv(GLuint program, GLenum pname, GLint* params)
{
    pb_push(SGL_CMD_GETPROGRAMIV);
    pb_push(program);
    pb_push(pname);

    glimpl_submit();
    *params = pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint* params)
{
    pb_push(SGL_CMD_GETSHADERIV);
    pb_push(shader);
    pb_push(pname);

    glimpl_submit();
    *params = pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glGetObjectParameterivARB(GLhandleARB obj, GLenum pname, GLint *params)
{
    pb_push(SGL_CMD_GETOBJECTPARAMETERIVARB);
    pb_push(obj);
    pb_push(pname);

    glimpl_submit();
    *params = pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

static inline void real_glGetString(GLenum name, char *string)
{
    pb_push(SGL_CMD_GETSTRING);
    pb_push(name);

    glimpl_submit();
    
    uintptr_t ptr = (uintptr_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V);
    memcpy(string, (void*)ptr, pb_read(SGL_OFFSET_REGISTER_RETVAL));
}

const GLubyte *glGetString(GLenum name)
{
    static char version[16] = "X.X.0";
    static char glsl_vr[5] = "X.X0";

    static char real_vendor[256] = "SharedGL and ";
    static char real_renderer[256] = "SharedGL using ";

    if (version[0] == 'X') {
        version[0] = '0' + (char)glimpl_major;
        version[2] = '0' + (char)glimpl_minor;
        
        char *glsl_override = getenv("GLSL_VERSION_OVERRIDE");

        if (glsl_override) {
            glsl_vr[0] = glsl_override[0];
            glsl_vr[2] = glsl_override[2];
        } 
        else {
            if ((glimpl_major == 3 && glimpl_minor == 3) || glimpl_major > 3) {
                glsl_vr[0] = version[0];
                glsl_vr[2] = version[2];
            }
            else {
                glsl_vr[0] = '3';
                glsl_vr[2] = '3';
            }
        }

        real_glGetString(GL_VENDOR, real_vendor + 13);
        real_glGetString(GL_RENDERER, real_renderer + 15);
    }

    switch (name) {
    case GL_VENDOR: return (const GLubyte *)real_vendor;
    case GL_RENDERER: return (const GLubyte *)real_renderer;
    case GL_VERSION: return (const GLubyte *)version;
    case GL_EXTENSIONS: return (const GLubyte *)glimpl_extensions_full;
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)glsl_vr;
    }
    return (const GLubyte *)"?";
}

const GLubyte *glGetStringi(GLenum name, GLuint index)
{
    return (name != GL_EXTENSIONS || index >= NUM_EXTENSIONS || index < 0) ? (const GLubyte *)"?" : (const GLubyte *)glimpl_extensions_list[index];
}

GLint glGetUniformLocation(GLuint program, const GLchar* name)
{
    pb_push(SGL_CMD_GETUNIFORMLOCATION);
    pb_push(program);
    push_string(name);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

GLint glGetAttribLocation(GLuint program, const GLchar* name)
{
    pb_push(SGL_CMD_GETATTRIBLOCATION);
    pb_push(program);
    push_string(name);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glGetFloatv(GLenum pname, GLfloat* data)
{
    pb_push(SGL_CMD_GETFLOATV);
    pb_push(pname);
    glimpl_submit();
    GL_GET_MEMCPY_RETVAL(data, float);
}

void glGetIntegerv(GLenum pname, GLint* data)
{
    /*
     * pre-check: these are name's we don't
     * want to pass through
     */
    switch (pname) {
    case GL_MAJOR_VERSION:
        data[0] = glimpl_major;
        return;
    case GL_MINOR_VERSION:
        data[0] = glimpl_minor;
        return;
    case GL_NUM_EXTENSIONS:
        data[0] = NUM_EXTENSIONS;
        return;
    }

    pb_push(SGL_CMD_GETINTEGERV);
    pb_push(pname);
    glimpl_submit();
    GL_GET_MEMCPY_RETVAL(data, int);
}

void glGetBooleanv(GLenum pname, GLboolean* data)
{
    pb_push(SGL_CMD_GETBOOLEANV);
    pb_push(pname);
    glimpl_submit();
    GL_GET_MEMCPY_RETVAL(data, GLboolean);
}

void glGetDoublev(GLenum pname, GLdouble* data)
{
    pb_push(SGL_CMD_GETDOUBLEV);
    pb_push(pname);
    glimpl_submit();
    GL_GET_MEMCPY_RETVAL(data, double);
}

void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* pixels)
{
    int width = 0, height = 0, depth = 0;
    switch (target) {
    case GL_TEXTURE_3D:
        glimpl_get_texture_level_parameter(SGL_CMD_GETTEXLEVELPARAMETERIV, target, level, GL_TEXTURE_DEPTH, &depth);
    case GL_TEXTURE_2D:
    case GL_TEXTURE_RECTANGLE:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
    case GL_TEXTURE_2D_ARRAY:
    case GL_TEXTURE_CUBE_MAP_ARRAY:
        glimpl_get_texture_level_parameter(SGL_CMD_GETTEXLEVELPARAMETERIV, target, level, GL_TEXTURE_HEIGHT, &height);
    case GL_TEXTURE_1D:
    case GL_TEXTURE_1D_ARRAY:
        glimpl_get_texture_level_parameter(SGL_CMD_GETTEXLEVELPARAMETERIV, target, level, GL_TEXTURE_WIDTH, &width);
        break;
    }

    if (!depth) depth = 1;
    if (!height) height = 1;
    if (!width) width = 1;

    size_t total_size = width * height * depth * glimpl_get_pixel_size(format);
    
    pb_push(SGL_CMD_GETTEXIMAGE);
    pb_push(target);
    pb_push(level);
    pb_push(format);
    pb_push(type);
    pb_push(total_size);

    glimpl_submit();
    glimpl_download_buffer(pixels, total_size);
}

void glLightModelfv(GLenum pname, const GLfloat* params)
{
    pb_push(SGL_CMD_LIGHTMODELFV);
    pb_push(pname);
    pb_pushf(params[0]);
    pb_pushf(params[1]); // to-do: check if this is causing an issues (overreading)
    pb_pushf(params[2]);
    pb_pushf(params[3]);
}

void glLightfv(GLenum light, GLenum pname, const GLfloat* params)
{
    pb_push(SGL_CMD_LIGHTFV);
    pb_push(light);
    pb_push(pname);
    pb_pushf(params[0]);
    pb_pushf(params[1]);
    pb_pushf(params[2]);
    pb_pushf(params[3]);
}

void glLinkProgram(GLuint program)
{
    pb_push(SGL_CMD_LINKPROGRAM);
    pb_push(program);
}

void glLoadIdentity(void)
{
    pb_push(SGL_CMD_LOADIDENTITY);
}

void glMaterialfv(GLenum face, GLenum pname, const GLfloat* params)
{
    pb_push(SGL_CMD_MATERIALFV);
    pb_push(face);
    pb_push(pname);
    pb_pushf(params[0]);
    pb_pushf(params[1]);
    pb_pushf(params[2]);
    pb_pushf(params[3]);
}

void glMatrixMode(GLenum mode)
{
    pb_push(SGL_CMD_MATRIXMODE);
    pb_push(mode);
}

void glNewList(GLuint list, GLenum mode)
{
    pb_push(SGL_CMD_NEWLIST);
    pb_push(list);
    pb_push(mode);
}

void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz)
{
    pb_push(SGL_CMD_NORMAL3F);
    pb_pushf(nx);
    pb_pushf(ny);
    pb_pushf(nz);
}

void glPopMatrix(void)
{
    pb_push(SGL_CMD_POPMATRIX);
}

void glPushMatrix(void)
{
    pb_push(SGL_CMD_PUSHMATRIX);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    pb_push(SGL_CMD_ROTATEF);
    pb_pushf(angle);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
}

void glShadeModel(GLenum mode)
{
    pb_push(SGL_CMD_SHADEMODEL);
    pb_push(mode);
}

void glShaderSource(GLuint shader, GLsizei count, const GLchar** string, const GLint* length)
{
    pb_push(SGL_CMD_SHADERSOURCE);
    pb_push(shader);
    pb_push(count);

    for (int i = 0; i < count; i++)
        push_string(string[i]);

    glimpl_submit();
}

void glTexImage1D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLint border, GLenum format, GLenum type, const void* pixels)
{
    glimpl_texture_image(SGL_CMD_TEXIMAGE1D, 1, 
            target, level, internalformat, width, 0, 0, border, format, type, pixels);
}

void glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const void* pixels)
{
    glimpl_texture_subimage(SGL_CMD_TEXSUBIMAGE1D, 1,
            target, level, xoffset, 0, 0, width, 0, 0, format, type, pixels);
}

void glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels)
{
    glimpl_texture_image(SGL_CMD_TEXIMAGE3D, 3, 
            target, level, internalformat, width, height, depth, border, format, type, pixels);
}

void glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels)
{
    glimpl_texture_subimage(SGL_CMD_TEXSUBIMAGE3D, 3,
            target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels)
{
    glimpl_texture_image(SGL_CMD_TEXIMAGE2D, 2, 
            target, level, internalformat, width, height, 0, border, format, type, pixels);
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels)
{
    glimpl_texture_subimage(SGL_CMD_TEXSUBIMAGE2D, 2,
            target, level, xoffset, yoffset, 0, width, height, 0, format, type, pixels);
}

void glTranslated(GLdouble x, GLdouble y, GLdouble z)
{
    pb_push(SGL_CMD_TRANSLATED);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    pb_push(SGL_CMD_TRANSLATEF);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
}

void glUniform1f(GLint location, GLfloat v0)
{
    pb_push(SGL_CMD_UNIFORM1F);
    pb_push(location);
    pb_pushf(v0);
}

void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 4 * 4);
    for (int i = 0; i < count * 4 * 4; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_UNIFORMMATRIX4FV);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glUseProgram(GLuint program)
{
    pb_push(SGL_CMD_USEPROGRAM);
    pb_push(program);
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z) 
{
    pb_push(SGL_CMD_VERTEX3F);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
}

void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)
{
    bool client_managed = !is_value_likely_an_offset(pointer);

    glimpl_vaps[index] = (struct gl_vertex_attrib_pointer){ 
        .index = index,
        .size = size,
        .type = type,
        .normalized = normalized,
        .stride = stride,
        .ptr = (void*)pointer,
        .enabled = false,
        .client_managed = client_managed
    };

    if (!client_managed) {
        pb_push(SGL_CMD_VERTEXATTRIBPOINTER);
        pb_push(index);
        pb_push(size);
        pb_push(type);
        pb_push(normalized);
        pb_push(stride);
        pb_push((int)((uintptr_t)pointer));
    }
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    pb_push(SGL_CMD_VIEWPORT);
    pb_push(x);
    pb_push(y);
    pb_push(width);
    pb_push(height);
    icd_resize(width, height);
}

void glMultMatrixd(const GLdouble* m)
{
    pb_push(SGL_CMD_MULTMATRIXF);
    for (int i = 0; i < 16; i++)
        pb_pushf(m[i]);
}

void glMultMatrixf(const GLfloat* m)
{
    pb_push(SGL_CMD_MULTMATRIXF);
    for (int i = 0; i < 16; i++)
        pb_pushf(m[i]);
}

void glLoadMatrixd(const GLdouble* m)
{
    pb_push(SGL_CMD_LOADMATRIXF);
    for (int i = 0; i < 16; i++)
        pb_pushf(m[i]);
}

void glLoadMatrixf(const GLfloat* m)
{
    pb_push(SGL_CMD_LOADMATRIXF);
    for (int i = 0; i < 16; i++)
        pb_pushf(m[i]);
}

void glColorPointer(GLint size, GLenum type, GLsizei stride, const void* pointer)
{
    glimpl_color_ptr = (struct gl_color_tex_vertex_pointer){
        .size = size,
        .type = type,
        .stride = stride,
        .pointer = pointer,
        .in_use = glimpl_color_ptr.in_use
    };
}

void glNormalPointer(GLenum type, GLsizei stride, const void* pointer)
{
    glimpl_normal_ptr = (struct gl_normal_index_pointer){
        .type = type,
        .stride = stride,
        .pointer = pointer,
        .in_use = glimpl_normal_ptr.in_use
    };
}

void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* pointer)
{
    glimpl_tex_coord_ptr[glimpl_client_active_texture] = (struct gl_color_tex_vertex_pointer){
        .size = size,
        .type = type,
        .stride = stride,
        .pointer = pointer,
        .in_use = glimpl_tex_coord_ptr[glimpl_client_active_texture].in_use
    };
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void* pointer)
{
    glimpl_vertex_ptr = (struct gl_color_tex_vertex_pointer){
        .size = size,
        .type = type,
        .stride = stride,
        .pointer = pointer,
        .in_use = glimpl_vertex_ptr.in_use
    };
}

void glEdgeFlagPointer(GLsizei stride, const void* pointer)
{
    glimpl_edge_flag_ptr = (struct gl_edge_flag_pointer){
        .stride = stride,
        .pointer = pointer,
        .in_use = glimpl_edge_flag_ptr.in_use
    };
}

void glIndexPointer(GLenum type, GLsizei stride, const void* pointer)
{
    glimpl_index_pointer = (struct gl_normal_index_pointer){
        .type = type,
        .stride = stride,
        .pointer = pointer,
        .in_use = glimpl_index_pointer.in_use
    };
}

void glInterleavedArrays(GLenum format, GLsizei stride, const void* pointer)
{
    glimpl_interleaved_pointer = (struct gl_normal_index_pointer){
        .type = format,
        .stride = stride,
        .pointer = pointer,
        .in_use = glimpl_interleaved_pointer.in_use
    };
}

void glFogCoordPointer(GLenum type, GLsizei stride, const void* pointer)
{
    glimpl_fog_coord_pointer = (struct gl_normal_index_pointer){
        .type = type,
        .stride = stride,
        .pointer = pointer,
        .in_use = glimpl_fog_coord_pointer.in_use
    };
}

void glSecondaryColorPointer(GLint size, GLenum type, GLsizei stride, const void* pointer)
{
    glimpl_color2_ptr = (struct gl_color_tex_vertex_pointer){
        .size = size,
        .type = type,
        .stride = stride,
        .pointer = pointer,
        .in_use = glimpl_color2_ptr.in_use
    };
}

void glCullFace(GLenum mode)
{
    pb_push(SGL_CMD_CULLFACE);
    pb_push(mode);
}

void glFrontFace(GLenum mode)
{
    pb_push(SGL_CMD_FRONTFACE);
    pb_push(mode);
}

void glHint(GLenum target, GLenum mode)
{
    pb_push(SGL_CMD_HINT);
    pb_push(target);
    pb_push(mode);
}

void glLineWidth(GLfloat width)
{
    pb_push(SGL_CMD_LINEWIDTH);
    pb_pushf(width);
}

void glPointSize(GLfloat size)
{
    pb_push(SGL_CMD_POINTSIZE);
    pb_pushf(size);
}

void glPolygonMode(GLenum face, GLenum mode)
{
    pb_push(SGL_CMD_POLYGONMODE);
    pb_push(face);
    pb_push(mode);
}

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
    pb_push(SGL_CMD_SCISSOR);
    pb_push(x);
    pb_push(y);
    pb_push(width);
    pb_push(height);

    /* 
     * icd hack for WineD3D bc it doesn't appear to call glViewport
     */
    if (x == 0 && y == 0 && width > 64 && height > 64)
        icd_resize(width, height);
}

void glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    pb_push(SGL_CMD_TEXPARAMETERF);
    pb_push(target);
    pb_push(pname);
    pb_pushf(param);
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    pb_push(SGL_CMD_TEXPARAMETERI);
    pb_push(target);
    pb_push(pname);
    pb_push(param);
}

void glClearStencil(GLint s)
{
    pb_push(SGL_CMD_CLEARSTENCIL);
    pb_push(s);
}

void glClearDepth(GLdouble depth)
{
    pb_push(SGL_CMD_CLEARDEPTH);
    pb_pushf(depth);
}

void glStencilMask(GLuint mask)
{
    pb_push(SGL_CMD_STENCILMASK);
    pb_push(mask);
}

void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)
{
    pb_push(SGL_CMD_COLORMASK);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
    pb_push(alpha);
}

void glDepthMask(GLboolean flag)
{
    pb_push(SGL_CMD_DEPTHMASK);
    pb_push(flag);
}

void glFinish(void)
{
    pb_push(SGL_CMD_FINISH);
}

void glFlush(void)
{
    pb_push(SGL_CMD_FLUSH);
}

void glLogicOp(GLenum opcode)
{
    pb_push(SGL_CMD_LOGICOP);
    pb_push(opcode);
}

void glStencilFunc(GLenum func, GLint ref, GLuint mask)
{
    pb_push(SGL_CMD_STENCILFUNC);
    pb_push(func);
    pb_push(ref);
    pb_push(mask);
}

void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{
    pb_push(SGL_CMD_STENCILOP);
    pb_push(fail);
    pb_push(zfail);
    pb_push(zpass);
}

void glPixelStoref(GLenum pname, GLfloat param)
{
    pb_push(SGL_CMD_PIXELSTOREF);
    pb_push(pname);
    pb_pushf(param);
}

void glPixelStorei(GLenum pname, GLint param)
{
    pb_push(SGL_CMD_PIXELSTOREI);
    pb_push(pname);
    pb_push(param);
}

void glReadBuffer(GLenum src)
{
    pb_push(SGL_CMD_READBUFFER);
    pb_push(src);
}

GLenum glGetError(void)
{
    return GL_NO_ERROR;
}

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels)
{
    STUB();
}

GLboolean glIsEnabled(GLenum cap)
{
    pb_push(SGL_CMD_ISENABLED);
    pb_push(cap);
    
    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glDepthRange(GLdouble n, GLdouble f)
{
    pb_push(SGL_CMD_DEPTHRANGE);
    pb_pushf(n);
    pb_pushf(f);
}

void glDeleteLists(GLuint list, GLsizei range)
{
    pb_push(SGL_CMD_DELETELISTS);
    pb_push(list);
    pb_push(range);
}

void glListBase(GLuint base)
{
    pb_push(SGL_CMD_LISTBASE);
    pb_push(base);
}

void glTexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations)
{
    pb_push(SGL_CMD_TEXIMAGE2DMULTISAMPLE);
    pb_push(target);
    pb_push(samples);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
    pb_push(fixedsamplelocations);
}

void glColor3b(GLbyte red, GLbyte green, GLbyte blue)
{
    pb_push(SGL_CMD_COLOR3B);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
}

void glColor3d(GLdouble red, GLdouble green, GLdouble blue)
{
    pb_push(SGL_CMD_COLOR3D);
    pb_pushf(red);
    pb_pushf(green);
    pb_pushf(blue);
}

void glColor3i(GLint red, GLint green, GLint blue)
{
    pb_push(SGL_CMD_COLOR3I);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
}

void glColor3s(GLshort red, GLshort green, GLshort blue)
{
    pb_push(SGL_CMD_COLOR3S);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
}

void glColor3ub(GLubyte red, GLubyte green, GLubyte blue)
{
    pb_push(SGL_CMD_COLOR3UB);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
}

void glColor3ui(GLuint red, GLuint green, GLuint blue)
{
    pb_push(SGL_CMD_COLOR3UI);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
}

void glColor3us(GLushort red, GLushort green, GLushort blue)
{
    pb_push(SGL_CMD_COLOR3US);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
}

void glColor4b(GLbyte red, GLbyte green, GLbyte blue, GLbyte alpha)
{
    pb_push(SGL_CMD_COLOR4B);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
    pb_push(alpha);
}

void glColor4d(GLdouble red, GLdouble green, GLdouble blue, GLdouble alpha)
{
    pb_push(SGL_CMD_COLOR4D);
    pb_pushf(red);
    pb_pushf(green);
    pb_pushf(blue);
    pb_pushf(alpha);
}

void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
    pb_push(SGL_CMD_COLOR4F);
    pb_pushf(red);
    pb_pushf(green);
    pb_pushf(blue);
    pb_pushf(alpha);
}

void glColor4fv(const GLfloat* v)
{
    pb_push(SGL_CMD_COLOR4F);
    pb_pushf(v[0]);
    pb_pushf(v[1]);
    pb_pushf(v[2]);
    pb_pushf(v[3]);
}

void glColor4i(GLint red, GLint green, GLint blue, GLint alpha)
{
    pb_push(SGL_CMD_COLOR4I);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
    pb_push(alpha);
}

void glColor4s(GLshort red, GLshort green, GLshort blue, GLshort alpha)
{
    pb_push(SGL_CMD_COLOR4S);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
    pb_push(alpha);
}

void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha)
{
    pb_push(SGL_CMD_COLOR4UB);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
    pb_push(alpha);
}

void glColor4ui(GLuint red, GLuint green, GLuint blue, GLuint alpha)
{
    pb_push(SGL_CMD_COLOR4UI);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
    pb_push(alpha);
}

void glColor4us(GLushort red, GLushort green, GLushort blue, GLushort alpha)
{
    pb_push(SGL_CMD_COLOR4US);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
    pb_push(alpha);
}

void glEdgeFlag(GLboolean flag)
{
    pb_push(SGL_CMD_EDGEFLAG);
    pb_push(flag);
}

void glIndexd(GLdouble c)
{
    pb_push(SGL_CMD_INDEXD);
    pb_pushf(c);
}

void glIndexf(GLfloat c)
{
    pb_push(SGL_CMD_INDEXF);
    pb_pushf(c);
}

void glIndexi(GLint c)
{
    pb_push(SGL_CMD_INDEXI);
    pb_push(c);
}

void glIndexs(GLshort c)
{
    pb_push(SGL_CMD_INDEXS);
    pb_push(c);
}

void glNormal3b(GLbyte nx, GLbyte ny, GLbyte nz)
{
    pb_push(SGL_CMD_NORMAL3B);
    pb_push(nx);
    pb_push(ny);
    pb_push(nz);
}

void glNormal3d(GLdouble nx, GLdouble ny, GLdouble nz)
{
    pb_push(SGL_CMD_NORMAL3D);
    pb_pushf(nx);
    pb_pushf(ny);
    pb_pushf(nz);
}

void glNormal3i(GLint nx, GLint ny, GLint nz)
{
    pb_push(SGL_CMD_NORMAL3I);
    pb_push(nx);
    pb_push(ny);
    pb_push(nz);
}

void glNormal3s(GLshort nx, GLshort ny, GLshort nz)
{
    pb_push(SGL_CMD_NORMAL3S);
    pb_push(nx);
    pb_push(ny);
    pb_push(nz);
}

void glRasterPos2d(GLdouble x, GLdouble y)
{
    pb_push(SGL_CMD_RASTERPOS2D);
    pb_pushf(x);
    pb_pushf(y);
}

void glRasterPos2f(GLfloat x, GLfloat y)
{
    pb_push(SGL_CMD_RASTERPOS2F);
    pb_pushf(x);
    pb_pushf(y);
}

void glRasterPos2i(GLint x, GLint y)
{
    pb_push(SGL_CMD_RASTERPOS2I);
    pb_push(x);
    pb_push(y);
}

void glRasterPos2s(GLshort x, GLshort y)
{
    pb_push(SGL_CMD_RASTERPOS2S);
    pb_push(x);
    pb_push(y);
}

void glRasterPos3d(GLdouble x, GLdouble y, GLdouble z)
{
    pb_push(SGL_CMD_RASTERPOS3D);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
}

void glRasterPos3f(GLfloat x, GLfloat y, GLfloat z)
{
    pb_push(SGL_CMD_RASTERPOS3F);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
}

void glRasterPos3i(GLint x, GLint y, GLint z)
{
    pb_push(SGL_CMD_RASTERPOS3I);
    pb_push(x);
    pb_push(y);
    pb_push(z);
}

void glRasterPos3s(GLshort x, GLshort y, GLshort z)
{
    pb_push(SGL_CMD_RASTERPOS3S);
    pb_push(x);
    pb_push(y);
    pb_push(z);
}

void glRasterPos4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w)
{
    pb_push(SGL_CMD_RASTERPOS4D);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
    pb_pushf(w);
}

void glRasterPos4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    pb_push(SGL_CMD_RASTERPOS4F);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
    pb_pushf(w);
}

void glRasterPos4i(GLint x, GLint y, GLint z, GLint w)
{
    pb_push(SGL_CMD_RASTERPOS4I);
    pb_push(x);
    pb_push(y);
    pb_push(z);
    pb_push(w);
}

void glRasterPos4s(GLshort x, GLshort y, GLshort z, GLshort w)
{
    pb_push(SGL_CMD_RASTERPOS4S);
    pb_push(x);
    pb_push(y);
    pb_push(z);
    pb_push(w);
}

void glRectd(GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2)
{
    pb_push(SGL_CMD_RECTD);
    pb_pushf(x1);
    pb_pushf(y1);
    pb_pushf(x2);
    pb_pushf(y2);
}

void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2)
{
    pb_push(SGL_CMD_RECTF);
    pb_pushf(x1);
    pb_pushf(y1);
    pb_pushf(x2);
    pb_pushf(y2);
}

void glRecti(GLint x1, GLint y1, GLint x2, GLint y2)
{
    pb_push(SGL_CMD_RECTI);
    pb_push(x1);
    pb_push(y1);
    pb_push(x2);
    pb_push(y2);
}

void glRects(GLshort x1, GLshort y1, GLshort x2, GLshort y2)
{
    pb_push(SGL_CMD_RECTS);
    pb_push(x1);
    pb_push(y1);
    pb_push(x2);
    pb_push(y2);
}

void glTexCoord1d(GLdouble s)
{
    pb_push(SGL_CMD_TEXCOORD1D);
    pb_pushf(s);
}

void glTexCoord1f(GLfloat s)
{
    pb_push(SGL_CMD_TEXCOORD1F);
    pb_pushf(s);
}

void glTexCoord1i(GLint s)
{
    pb_push(SGL_CMD_TEXCOORD1I);
    pb_push(s);
}

void glTexCoord1s(GLshort s)
{
    pb_push(SGL_CMD_TEXCOORD1S);
    pb_push(s);
}

void glTexCoord2d(GLdouble s, GLdouble t)
{
    pb_push(SGL_CMD_TEXCOORD2D);
    pb_pushf(s);
    pb_pushf(t);
}

void glTexCoord2f(GLfloat s, GLfloat t)
{
    pb_push(SGL_CMD_TEXCOORD2F);
    pb_pushf(s);
    pb_pushf(t);
}

void glTexCoord2i(GLint s, GLint t)
{
    pb_push(SGL_CMD_TEXCOORD2I);
    pb_push(s);
    pb_push(t);
}

void glTexCoord2s(GLshort s, GLshort t)
{
    pb_push(SGL_CMD_TEXCOORD2S);
    pb_push(s);
    pb_push(t);
}

void glTexCoord3d(GLdouble s, GLdouble t, GLdouble r)
{
    pb_push(SGL_CMD_TEXCOORD3D);
    pb_pushf(s);
    pb_pushf(t);
    pb_pushf(r);
}

void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r)
{
    pb_push(SGL_CMD_TEXCOORD3F);
    pb_pushf(s);
    pb_pushf(t);
    pb_pushf(r);
}

void glTexCoord3i(GLint s, GLint t, GLint r)
{
    pb_push(SGL_CMD_TEXCOORD3I);
    pb_push(s);
    pb_push(t);
    pb_push(r);
}

void glTexCoord3s(GLshort s, GLshort t, GLshort r)
{
    pb_push(SGL_CMD_TEXCOORD3S);
    pb_push(s);
    pb_push(t);
    pb_push(r);
}

void glTexCoord4d(GLdouble s, GLdouble t, GLdouble r, GLdouble q)
{
    pb_push(SGL_CMD_TEXCOORD4D);
    pb_pushf(s);
    pb_pushf(t);
    pb_pushf(r);
    pb_pushf(q);
}

void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q)
{
    pb_push(SGL_CMD_TEXCOORD4F);
    pb_pushf(s);
    pb_pushf(t);
    pb_pushf(r);
    pb_pushf(q);
}

void glTexCoord4i(GLint s, GLint t, GLint r, GLint q)
{
    pb_push(SGL_CMD_TEXCOORD4I);
    pb_push(s);
    pb_push(t);
    pb_push(r);
    pb_push(q);
}

void glTexCoord4s(GLshort s, GLshort t, GLshort r, GLshort q)
{
    pb_push(SGL_CMD_TEXCOORD4S);
    pb_push(s);
    pb_push(t);
    pb_push(r);
    pb_push(q);
}

void glVertex2d(GLdouble x, GLdouble y)
{
    pb_push(SGL_CMD_VERTEX2D);
    pb_pushf(x);
    pb_pushf(y);
}

void glVertex2f(GLfloat x, GLfloat y)
{
    pb_push(SGL_CMD_VERTEX2F);
    pb_pushf(x);
    pb_pushf(y);
}

void glVertex2i(GLint x, GLint y)
{
    pb_push(SGL_CMD_VERTEX2I);
    pb_push(x);
    pb_push(y);
}

void glVertex2s(GLshort x, GLshort y)
{
    pb_push(SGL_CMD_VERTEX2S);
    pb_push(x);
    pb_push(y);
}

void glVertex3d(GLdouble x, GLdouble y, GLdouble z)
{
    pb_push(SGL_CMD_VERTEX3D);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
}

void glVertex3i(GLint x, GLint y, GLint z)
{
    pb_push(SGL_CMD_VERTEX3I);
    pb_push(x);
    pb_push(y);
    pb_push(z);
}

void glVertex3s(GLshort x, GLshort y, GLshort z)
{
    pb_push(SGL_CMD_VERTEX3S);
    pb_push(x);
    pb_push(y);
    pb_push(z);
}

void glVertex4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w)
{
    pb_push(SGL_CMD_VERTEX4D);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
    pb_pushf(w);
}

void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    pb_push(SGL_CMD_VERTEX4F);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
    pb_pushf(w);
}

void glVertex4i(GLint x, GLint y, GLint z, GLint w)
{
    pb_push(SGL_CMD_VERTEX4I);
    pb_push(x);
    pb_push(y);
    pb_push(z);
    pb_push(w);
}

void glVertex4s(GLshort x, GLshort y, GLshort z, GLshort w)
{
    pb_push(SGL_CMD_VERTEX4S);
    pb_push(x);
    pb_push(y);
    pb_push(z);
    pb_push(w);
}

void glColorMaterial(GLenum face, GLenum mode)
{
    pb_push(SGL_CMD_COLORMATERIAL);
    pb_push(face);
    pb_push(mode);
}

void glFogf(GLenum pname, GLfloat param)
{
    pb_push(SGL_CMD_FOGF);
    pb_push(pname);
    pb_pushf(param);
}

void glFogfv(GLenum pname, const GLfloat* params)
{
    if (pname != GL_FOG_COLOR)
        glFogf(pname, params[0]);
    else {
        pb_push(SGL_CMD_FOGFV);
        pb_push(pname);
        pb_pushf(params[0]);
        pb_pushf(params[1]);
        pb_pushf(params[2]);
        pb_pushf(params[3]);
    }
}

void glFogi(GLenum pname, GLint param)
{
    pb_push(SGL_CMD_FOGI);
    pb_push(pname);
    pb_push(param);
}

void glLightf(GLenum light, GLenum pname, GLfloat param)
{
    pb_push(SGL_CMD_LIGHTF);
    pb_push(light);
    pb_push(pname);
    pb_pushf(param);
}

void glLighti(GLenum light, GLenum pname, GLint param)
{
    pb_push(SGL_CMD_LIGHTI);
    pb_push(light);
    pb_push(pname);
    pb_push(param);
}

void glLightModelf(GLenum pname, GLfloat param)
{
    pb_push(SGL_CMD_LIGHTMODELF);
    pb_push(pname);
    pb_pushf(param);
}

void glLightModeli(GLenum pname, GLint param)
{
    pb_push(SGL_CMD_LIGHTMODELI);
    pb_push(pname);
    pb_push(param);
}

void glLineStipple(GLint factor, GLushort pattern)
{
    pb_push(SGL_CMD_LINESTIPPLE);
    pb_push(factor);
    pb_push(pattern);
}

void glMaterialf(GLenum face, GLenum pname, GLfloat param)
{
    pb_push(SGL_CMD_MATERIALF);
    pb_push(face);
    pb_push(pname);
    pb_pushf(param);
}

void glMateriali(GLenum face, GLenum pname, GLint param)
{
    pb_push(SGL_CMD_MATERIALI);
    pb_push(face);
    pb_push(pname);
    pb_push(param);
}

void glTexEnvf(GLenum target, GLenum pname, GLfloat param)
{
    pb_push(SGL_CMD_TEXENVF);
    pb_push(target);
    pb_push(pname);
    pb_pushf(param);
}

void glTexEnvi(GLenum target, GLenum pname, GLint param)
{
    pb_push(SGL_CMD_TEXENVI);
    pb_push(target);
    pb_push(pname);
    pb_push(param);
}

void glTexGend(GLenum coord, GLenum pname, GLdouble param)
{
    pb_push(SGL_CMD_TEXGEND);
    pb_push(coord);
    pb_push(pname);
    pb_pushf(param);
}

void glTexGenf(GLenum coord, GLenum pname, GLfloat param)
{
    pb_push(SGL_CMD_TEXGENF);
    pb_push(coord);
    pb_push(pname);
    pb_pushf(param);
}

void glTexGeni(GLenum coord, GLenum pname, GLint param)
{
    pb_push(SGL_CMD_TEXGENI);
    pb_push(coord);
    pb_push(pname);
    pb_push(param);
}

GLint glRenderMode(GLenum mode)
{
    pb_push(SGL_CMD_RENDERMODE);
    pb_push(mode);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glInitNames(void)
{
    pb_push(SGL_CMD_INITNAMES);
}

void glLoadName(GLuint name)
{
    pb_push(SGL_CMD_LOADNAME);
    pb_push(name);
}

void glPassThrough(GLfloat token)
{
    pb_push(SGL_CMD_PASSTHROUGH);
    pb_pushf(token);
}

void glPopName(void)
{
    pb_push(SGL_CMD_POPNAME);
}

void glPushName(GLuint name)
{
    pb_push(SGL_CMD_PUSHNAME);
    pb_push(name);
}

void glClearAccum(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
    pb_push(SGL_CMD_CLEARACCUM);
    pb_pushf(red);
    pb_pushf(green);
    pb_pushf(blue);
    pb_pushf(alpha);
}

void glClearIndex(GLfloat c)
{
    pb_push(SGL_CMD_CLEARINDEX);
    pb_pushf(c);
}

void glIndexMask(GLuint mask)
{
    pb_push(SGL_CMD_INDEXMASK);
    pb_push(mask);
}

void glAccum(GLenum op, GLfloat value)
{
    pb_push(SGL_CMD_ACCUM);
    pb_push(op);
    pb_pushf(value);
}

void glPopAttrib(void)
{
    pb_push(SGL_CMD_POPATTRIB);
}

void glPushAttrib(GLbitfield mask)
{
    pb_push(SGL_CMD_PUSHATTRIB);
    pb_push(mask);
}

void glMapGrid1d(GLint un, GLdouble u1, GLdouble u2)
{
    pb_push(SGL_CMD_MAPGRID1D);
    pb_push(un);
    pb_pushf(u1);
    pb_pushf(u2);
}

void glMapGrid1f(GLint un, GLfloat u1, GLfloat u2)
{
    pb_push(SGL_CMD_MAPGRID1F);
    pb_push(un);
    pb_pushf(u1);
    pb_pushf(u2);
}

void glMapGrid2d(GLint un, GLdouble u1, GLdouble u2, GLint vn, GLdouble v1, GLdouble v2)
{
    pb_push(SGL_CMD_MAPGRID2D);
    pb_push(un);
    pb_pushf(u1);
    pb_pushf(u2);
    pb_push(vn);
    pb_pushf(v1);
    pb_pushf(v2);
}

void glMapGrid2f(GLint un, GLfloat u1, GLfloat u2, GLint vn, GLfloat v1, GLfloat v2)
{
    pb_push(SGL_CMD_MAPGRID2F);
    pb_push(un);
    pb_pushf(u1);
    pb_pushf(u2);
    pb_push(vn);
    pb_pushf(v1);
    pb_pushf(v2);
}

void glEvalCoord1d(GLdouble u)
{
    pb_push(SGL_CMD_EVALCOORD1D);
    pb_pushf(u);
}

void glEvalCoord1f(GLfloat u)
{
    pb_push(SGL_CMD_EVALCOORD1F);
    pb_pushf(u);
}

void glEvalCoord2d(GLdouble u, GLdouble v)
{
    pb_push(SGL_CMD_EVALCOORD2D);
    pb_pushf(u);
    pb_pushf(v);
}

void glEvalCoord2f(GLfloat u, GLfloat v)
{
    pb_push(SGL_CMD_EVALCOORD2F);
    pb_pushf(u);
    pb_pushf(v);
}

void glEvalMesh1(GLenum mode, GLint i1, GLint i2)
{
    pb_push(SGL_CMD_EVALMESH1);
    pb_push(mode);
    pb_push(i1);
    pb_push(i2);
}

void glEvalPoint1(GLint i)
{
    pb_push(SGL_CMD_EVALPOINT1);
    pb_push(i);
}

void glEvalMesh2(GLenum mode, GLint i1, GLint i2, GLint j1, GLint j2)
{
    pb_push(SGL_CMD_EVALMESH2);
    pb_push(mode);
    pb_push(i1);
    pb_push(i2);
    pb_push(j1);
    pb_push(j2);
}

void glEvalPoint2(GLint i, GLint j)
{
    pb_push(SGL_CMD_EVALPOINT2);
    pb_push(i);
    pb_push(j);
}

void glAlphaFunc(GLenum func, GLfloat ref)
{
    pb_push(SGL_CMD_ALPHAFUNC);
    pb_push(func);
    pb_pushf(ref);
}

void glPixelZoom(GLfloat xfactor, GLfloat yfactor)
{
    pb_push(SGL_CMD_PIXELZOOM);
    pb_pushf(xfactor);
    pb_pushf(yfactor);
}

void glPixelTransferf(GLenum pname, GLfloat param)
{
    pb_push(SGL_CMD_PIXELTRANSFERF);
    pb_push(pname);
    pb_pushf(param);
}

void glPixelTransferi(GLenum pname, GLint param)
{
    pb_push(SGL_CMD_PIXELTRANSFERI);
    pb_push(pname);
    pb_push(param);
}

void glCopyPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type)
{
    pb_push(SGL_CMD_COPYPIXELS);
    pb_push(x);
    pb_push(y);
    pb_push(width);
    pb_push(height);
    pb_push(type);
}

GLboolean glIsList(GLuint list)
{
    pb_push(SGL_CMD_ISLIST);
    pb_push(list);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar)
{
    pb_push(SGL_CMD_ORTHO);
    pb_pushf(left);
    pb_pushf(right);
    pb_pushf(bottom);
    pb_pushf(top);
    pb_pushf(zNear);
    pb_pushf(zFar);
}

void glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z)
{
    pb_push(SGL_CMD_ROTATED);
    pb_pushf(angle);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
}

void glScaled(GLdouble x, GLdouble y, GLdouble z)
{
    pb_push(SGL_CMD_SCALED);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    pb_push(SGL_CMD_SCALEF);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
}

void glPolygonOffset(GLfloat factor, GLfloat units)
{
    pb_push(SGL_CMD_POLYGONOFFSET);
    pb_pushf(factor);
    pb_pushf(units);
}

void glCopyTexImage1D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLint border)
{
    pb_push(SGL_CMD_COPYTEXIMAGE1D);
    pb_push(target);
    pb_push(level);
    pb_push(internalformat);
    pb_push(x);
    pb_push(y);
    pb_push(width);
    pb_push(border);
}

void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border)
{
    pb_push(SGL_CMD_COPYTEXIMAGE2D);
    pb_push(target);
    pb_push(level);
    pb_push(internalformat);
    pb_push(x);
    pb_push(y);
    pb_push(width);
    pb_push(height);
    pb_push(border);
}

void glCopyTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width)
{
    pb_push(SGL_CMD_COPYTEXSUBIMAGE1D);
    pb_push(target);
    pb_push(level);
    pb_push(xoffset);
    pb_push(x);
    pb_push(y);
    pb_push(width);
}

void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
    pb_push(SGL_CMD_COPYTEXSUBIMAGE2D);
    pb_push(target);
    pb_push(level);
    pb_push(xoffset);
    pb_push(yoffset);
    pb_push(x);
    pb_push(y);
    pb_push(width);
    pb_push(height);
}

void glBindTexture(GLenum target, GLuint texture)
{
    pb_push(SGL_CMD_BINDTEXTURE);
    pb_push(target);
    pb_push(texture);
}

GLboolean glIsTexture(GLuint texture)
{
    pb_push(SGL_CMD_ISTEXTURE);
    pb_push(texture);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glArrayElement(GLint i)
{
    pb_push(SGL_CMD_ARRAYELEMENT);
    pb_push(i);
}

void glDisableClientState(GLenum array)
{
    switch (array) {
    case GL_COLOR_ARRAY:
        glimpl_color_ptr.in_use = false;
        break;
    case GL_NORMAL_ARRAY:
        glimpl_normal_ptr.in_use = false;
        break;
    case GL_TEXTURE_COORD_ARRAY:
        glimpl_tex_coord_ptr[glimpl_client_active_texture].in_use = false;
        break;
    case GL_VERTEX_ARRAY:
        glimpl_vertex_ptr.in_use = false;
        break;
    }

    pb_push(SGL_CMD_DISABLECLIENTSTATE);
    pb_push(array);
}

void glEnableClientState(GLenum array)
{
    switch (array) {
    case GL_COLOR_ARRAY:
        glimpl_color_ptr.in_use = true;
        break;
    case GL_NORMAL_ARRAY:
        glimpl_normal_ptr.in_use = true;
        break;
    case GL_TEXTURE_COORD_ARRAY:
        glimpl_tex_coord_ptr[glimpl_client_active_texture].in_use = true;
        break;
    case GL_VERTEX_ARRAY:
        glimpl_vertex_ptr.in_use = true;
        break;
    }

    pb_push(SGL_CMD_ENABLECLIENTSTATE);
    pb_push(array);
}

void glIndexub(GLubyte c)
{
    pb_push(SGL_CMD_INDEXUB);
    pb_push(c);
}

void glPopClientAttrib(void)
{
    pb_push(SGL_CMD_POPCLIENTATTRIB);
}

void glPushClientAttrib(GLbitfield mask)
{
    pb_push(SGL_CMD_PUSHCLIENTATTRIB);
    pb_push(mask);
}

void glTexImage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedsamplelocations)
{
    pb_push(SGL_CMD_TEXIMAGE3DMULTISAMPLE);
    pb_push(target);
    pb_push(samples);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
    pb_push(depth);
    pb_push(fixedsamplelocations);
}

void glCopyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
    pb_push(SGL_CMD_COPYTEXSUBIMAGE3D);
    pb_push(target);
    pb_push(level);
    pb_push(xoffset);
    pb_push(yoffset);
    pb_push(zoffset);
    pb_push(x);
    pb_push(y);
    pb_push(width);
    pb_push(height);
}

void glActiveTexture(GLenum texture)
{
    pb_push(SGL_CMD_ACTIVETEXTURE);
    pb_push(texture);
}

void glSampleCoverage(GLfloat value, GLboolean invert)
{
    pb_push(SGL_CMD_SAMPLECOVERAGE);
    pb_pushf(value);
    pb_push(invert);
}

void glClientActiveTexture(GLenum texture)
{
    pb_push(SGL_CMD_CLIENTACTIVETEXTURE);
    pb_push(texture);

    glimpl_client_active_texture = texture - (GLenum)GL_TEXTURE0;
}

void glClientActiveTextureARB(GLenum texture)
{
    glClientActiveTexture(texture);
}

void glMultiTexCoord1d(GLenum target, GLdouble s)
{
    pb_push(SGL_CMD_MULTITEXCOORD1D);
    pb_push(target);
    pb_pushf(s);
}

void glMultiTexCoord1f(GLenum target, GLfloat s)
{
    pb_push(SGL_CMD_MULTITEXCOORD1F);
    pb_push(target);
    pb_pushf(s);
}

void glMultiTexCoord1i(GLenum target, GLint s)
{
    pb_push(SGL_CMD_MULTITEXCOORD1I);
    pb_push(target);
    pb_push(s);
}

void glMultiTexCoord1s(GLenum target, GLshort s)
{
    pb_push(SGL_CMD_MULTITEXCOORD1S);
    pb_push(target);
    pb_push(s);
}

void glMultiTexCoord2d(GLenum target, GLdouble s, GLdouble t)
{
    pb_push(SGL_CMD_MULTITEXCOORD2D);
    pb_push(target);
    pb_pushf(s);
    pb_pushf(t);
}

void glMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t)
{
    pb_push(SGL_CMD_MULTITEXCOORD2F);
    pb_push(target);
    pb_pushf(s);
    pb_pushf(t);
}

void glMultiTexCoord2i(GLenum target, GLint s, GLint t)
{
    pb_push(SGL_CMD_MULTITEXCOORD2I);
    pb_push(target);
    pb_push(s);
    pb_push(t);
}

void glMultiTexCoord2s(GLenum target, GLshort s, GLshort t)
{
    pb_push(SGL_CMD_MULTITEXCOORD2S);
    pb_push(target);
    pb_push(s);
    pb_push(t);
}

void glMultiTexCoord3d(GLenum target, GLdouble s, GLdouble t, GLdouble r)
{
    pb_push(SGL_CMD_MULTITEXCOORD3D);
    pb_push(target);
    pb_pushf(s);
    pb_pushf(t);
    pb_pushf(r);
}

void glMultiTexCoord3f(GLenum target, GLfloat s, GLfloat t, GLfloat r)
{
    pb_push(SGL_CMD_MULTITEXCOORD3F);
    pb_push(target);
    pb_pushf(s);
    pb_pushf(t);
    pb_pushf(r);
}

void glMultiTexCoord3i(GLenum target, GLint s, GLint t, GLint r)
{
    pb_push(SGL_CMD_MULTITEXCOORD3I);
    pb_push(target);
    pb_push(s);
    pb_push(t);
    pb_push(r);
}

void glMultiTexCoord3s(GLenum target, GLshort s, GLshort t, GLshort r)
{
    pb_push(SGL_CMD_MULTITEXCOORD3S);
    pb_push(target);
    pb_push(s);
    pb_push(t);
    pb_push(r);
}

void glMultiTexCoord4d(GLenum target, GLdouble s, GLdouble t, GLdouble r, GLdouble q)
{
    pb_push(SGL_CMD_MULTITEXCOORD4D);
    pb_push(target);
    pb_pushf(s);
    pb_pushf(t);
    pb_pushf(r);
    pb_pushf(q);
}

void glMultiTexCoord4f(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q)
{
    pb_push(SGL_CMD_MULTITEXCOORD4F);
    pb_push(target);
    pb_pushf(s);
    pb_pushf(t);
    pb_pushf(r);
    pb_pushf(q);
}

void glMultiTexCoord4i(GLenum target, GLint s, GLint t, GLint r, GLint q)
{
    pb_push(SGL_CMD_MULTITEXCOORD4I);
    pb_push(target);
    pb_push(s);
    pb_push(t);
    pb_push(r);
    pb_push(q);
}

void glMultiTexCoord4s(GLenum target, GLshort s, GLshort t, GLshort r, GLshort q)
{
    pb_push(SGL_CMD_MULTITEXCOORD4S);
    pb_push(target);
    pb_push(s);
    pb_push(t);
    pb_push(r);
    pb_push(q);
}

void glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha)
{
    pb_push(SGL_CMD_BLENDFUNCSEPARATE);
    pb_push(sfactorRGB);
    pb_push(dfactorRGB);
    pb_push(sfactorAlpha);
    pb_push(dfactorAlpha);
}

void glPointParameterf(GLenum pname, GLfloat param)
{
    pb_push(SGL_CMD_POINTPARAMETERF);
    pb_push(pname);
    pb_pushf(param);
}

void glPointParameteri(GLenum pname, GLint param)
{
    pb_push(SGL_CMD_POINTPARAMETERI);
    pb_push(pname);
    pb_push(param);
}

void glFogCoordf(GLfloat coord)
{
    pb_push(SGL_CMD_FOGCOORDF);
    pb_pushf(coord);
}

void glFogCoordd(GLdouble coord)
{
    pb_push(SGL_CMD_FOGCOORDD);
    pb_pushf(coord);
}

void glSecondaryColor3b(GLbyte red, GLbyte green, GLbyte blue)
{
    pb_push(SGL_CMD_SECONDARYCOLOR3B);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
}

void glSecondaryColor3d(GLdouble red, GLdouble green, GLdouble blue)
{
    pb_push(SGL_CMD_SECONDARYCOLOR3D);
    pb_pushf(red);
    pb_pushf(green);
    pb_pushf(blue);
}

void glSecondaryColor3f(GLfloat red, GLfloat green, GLfloat blue)
{
    pb_push(SGL_CMD_SECONDARYCOLOR3F);
    pb_pushf(red);
    pb_pushf(green);
    pb_pushf(blue);
}

void glSecondaryColor3i(GLint red, GLint green, GLint blue)
{
    pb_push(SGL_CMD_SECONDARYCOLOR3I);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
}

void glSecondaryColor3s(GLshort red, GLshort green, GLshort blue)
{
    pb_push(SGL_CMD_SECONDARYCOLOR3S);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
}

void glSecondaryColor3ub(GLubyte red, GLubyte green, GLubyte blue)
{
    pb_push(SGL_CMD_SECONDARYCOLOR3UB);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
}

void glSecondaryColor3ui(GLuint red, GLuint green, GLuint blue)
{
    pb_push(SGL_CMD_SECONDARYCOLOR3UI);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
}

void glSecondaryColor3us(GLushort red, GLushort green, GLushort blue)
{
    pb_push(SGL_CMD_SECONDARYCOLOR3US);
    pb_push(red);
    pb_push(green);
    pb_push(blue);
}

void glWindowPos2d(GLdouble x, GLdouble y)
{
    pb_push(SGL_CMD_WINDOWPOS2D);
    pb_pushf(x);
    pb_pushf(y);
}

void glWindowPos2f(GLfloat x, GLfloat y)
{
    pb_push(SGL_CMD_WINDOWPOS2F);
    pb_pushf(x);
    pb_pushf(y);
}

void glWindowPos2i(GLint x, GLint y)
{
    pb_push(SGL_CMD_WINDOWPOS2I);
    pb_push(x);
    pb_push(y);
}

void glWindowPos2s(GLshort x, GLshort y)
{
    pb_push(SGL_CMD_WINDOWPOS2S);
    pb_push(x);
    pb_push(y);
}

void glWindowPos3d(GLdouble x, GLdouble y, GLdouble z)
{
    pb_push(SGL_CMD_WINDOWPOS3D);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
}

void glWindowPos3f(GLfloat x, GLfloat y, GLfloat z)
{
    pb_push(SGL_CMD_WINDOWPOS3F);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
}

void glWindowPos3i(GLint x, GLint y, GLint z)
{
    pb_push(SGL_CMD_WINDOWPOS3I);
    pb_push(x);
    pb_push(y);
    pb_push(z);
}

void glWindowPos3s(GLshort x, GLshort y, GLshort z)
{
    pb_push(SGL_CMD_WINDOWPOS3S);
    pb_push(x);
    pb_push(y);
    pb_push(z);
}

void glBlendColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
    pb_push(SGL_CMD_BLENDCOLOR);
    pb_pushf(red);
    pb_pushf(green);
    pb_pushf(blue);
    pb_pushf(alpha);
}

void glBlendEquation(GLenum mode)
{
    pb_push(SGL_CMD_BLENDEQUATION);
    pb_push(mode);
}

GLboolean glIsQuery(GLuint id)
{
    pb_push(SGL_CMD_ISQUERY);
    pb_push(id);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

GLboolean glIsBuffer(GLuint buffer)
{
    pb_push(SGL_CMD_ISBUFFER);
    pb_push(buffer);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

GLboolean glUnmapBuffer(GLenum target)
{
    return glimpl_unmap_buffer(SGL_CMD_UNMAPBUFFER, target);
}

void glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha)
{
    pb_push(SGL_CMD_BLENDEQUATIONSEPARATE);
    pb_push(modeRGB);
    pb_push(modeAlpha);
}

void glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass)
{
    pb_push(SGL_CMD_STENCILOPSEPARATE);
    pb_push(face);
    pb_push(sfail);
    pb_push(dpfail);
    pb_push(dppass);
}

void glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask)
{
    pb_push(SGL_CMD_STENCILFUNCSEPARATE);
    pb_push(face);
    pb_push(func);
    pb_push(ref);
    pb_push(mask);
}

void glStencilMaskSeparate(GLenum face, GLuint mask)
{
    pb_push(SGL_CMD_STENCILMASKSEPARATE);
    pb_push(face);
    pb_push(mask);
}

void glDisableVertexAttribArray(GLuint index)
{
    glimpl_vaps[index].enabled = false;

    // if (!glimpl_vaps[index].client_managed) {
        pb_push(SGL_CMD_DISABLEVERTEXATTRIBARRAY);
        pb_push(index);
    // }
}

GLboolean glIsProgram(GLuint program)
{
    pb_push(SGL_CMD_ISPROGRAM);
    pb_push(program);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

GLboolean glIsShader(GLuint shader)
{
    pb_push(SGL_CMD_ISSHADER);
    pb_push(shader);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glUniform2f(GLint location, GLfloat v0, GLfloat v1)
{
    pb_push(SGL_CMD_UNIFORM2F);
    pb_push(location);
    pb_pushf(v0);
    pb_pushf(v1);
}

void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2)
{
    pb_push(SGL_CMD_UNIFORM3F);
    pb_push(location);
    pb_pushf(v0);
    pb_pushf(v1);
    pb_pushf(v2);
}

void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)
{
    pb_push(SGL_CMD_UNIFORM4F);
    pb_push(location);
    pb_pushf(v0);
    pb_pushf(v1);
    pb_pushf(v2);
    pb_pushf(v3);
}

void glUniform1i(GLint location, GLint v0)
{
    pb_push(SGL_CMD_UNIFORM1I);
    pb_push(location);
    pb_push(v0);
}

void glUniform2i(GLint location, GLint v0, GLint v1)
{
    pb_push(SGL_CMD_UNIFORM2I);
    pb_push(location);
    pb_push(v0);
    pb_push(v1);
}

void glUniform3i(GLint location, GLint v0, GLint v1, GLint v2)
{
    pb_push(SGL_CMD_UNIFORM3I);
    pb_push(location);
    pb_push(v0);
    pb_push(v1);
    pb_push(v2);
}

void glUniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3)
{
    pb_push(SGL_CMD_UNIFORM4I);
    pb_push(location);
    pb_push(v0);
    pb_push(v1);
    pb_push(v2);
    pb_push(v3);
}

void glValidateProgram(GLuint program)
{
    pb_push(SGL_CMD_VALIDATEPROGRAM);
    pb_push(program);
}

void glVertexAttrib1d(GLuint index, GLdouble x)
{
    pb_push(SGL_CMD_VERTEXATTRIB1D);
    pb_push(index);
    pb_pushf(x);
}

void glVertexAttrib1f(GLuint index, GLfloat x)
{
    pb_push(SGL_CMD_VERTEXATTRIB1F);
    pb_push(index);
    pb_pushf(x);
}

void glVertexAttrib1s(GLuint index, GLshort x)
{
    pb_push(SGL_CMD_VERTEXATTRIB1S);
    pb_push(index);
    pb_push(x);
}

void glVertexAttrib2d(GLuint index, GLdouble x, GLdouble y)
{
    pb_push(SGL_CMD_VERTEXATTRIB2D);
    pb_push(index);
    pb_pushf(x);
    pb_pushf(y);
}

void glVertexAttrib2f(GLuint index, GLfloat x, GLfloat y)
{
    pb_push(SGL_CMD_VERTEXATTRIB2F);
    pb_push(index);
    pb_pushf(x);
    pb_pushf(y);
}

void glVertexAttrib2s(GLuint index, GLshort x, GLshort y)
{
    pb_push(SGL_CMD_VERTEXATTRIB2S);
    pb_push(index);
    pb_push(x);
    pb_push(y);
}

void glVertexAttrib3d(GLuint index, GLdouble x, GLdouble y, GLdouble z)
{
    pb_push(SGL_CMD_VERTEXATTRIB3D);
    pb_push(index);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
}

void glVertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z)
{
    pb_push(SGL_CMD_VERTEXATTRIB3F);
    pb_push(index);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
}

void glVertexAttrib3s(GLuint index, GLshort x, GLshort y, GLshort z)
{
    pb_push(SGL_CMD_VERTEXATTRIB3S);
    pb_push(index);
    pb_push(x);
    pb_push(y);
    pb_push(z);
}

void glVertexAttrib4Nub(GLuint index, GLubyte x, GLubyte y, GLubyte z, GLubyte w)
{
    pb_push(SGL_CMD_VERTEXATTRIB4NUB);
    pb_push(index);
    pb_push(x);
    pb_push(y);
    pb_push(z);
    pb_push(w);
}

void glVertexAttrib4d(GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w)
{
    pb_push(SGL_CMD_VERTEXATTRIB4D);
    pb_push(index);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
    pb_pushf(w);
}

void glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    pb_push(SGL_CMD_VERTEXATTRIB4F);
    pb_push(index);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
    pb_pushf(w);
}

void glVertexAttrib4s(GLuint index, GLshort x, GLshort y, GLshort z, GLshort w)
{
    pb_push(SGL_CMD_VERTEXATTRIB4S);
    pb_push(index);
    pb_push(x);
    pb_push(y);
    pb_push(z);
    pb_push(w);
}

void glColorMaski(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    pb_push(SGL_CMD_COLORMASKI);
    pb_push(index);
    pb_push(r);
    pb_push(g);
    pb_push(b);
    pb_push(a);
}

void glEnablei(GLenum target, GLuint index)
{
    pb_push(SGL_CMD_ENABLEI);
    pb_push(target);
    pb_push(index);
}

void glDisablei(GLenum target, GLuint index)
{
    pb_push(SGL_CMD_DISABLEI);
    pb_push(target);
    pb_push(index);
}

GLboolean glIsEnabledi(GLenum target, GLuint index)
{
    pb_push(SGL_CMD_ISENABLEDI);
    pb_push(target);
    pb_push(index);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glBeginTransformFeedback(GLenum primitiveMode)
{
    pb_push(SGL_CMD_BEGINTRANSFORMFEEDBACK);
    pb_push(primitiveMode);
}

void glEndTransformFeedback(void)
{
    pb_push(SGL_CMD_ENDTRANSFORMFEEDBACK);
}

void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size)
{
    pb_push(SGL_CMD_BINDBUFFERRANGE);
    pb_push(target);
    pb_push(index);
    pb_push(buffer);
    pb_push(offset);
    pb_push(size);
}

void glBindBufferBase(GLenum target, GLuint index, GLuint buffer)
{
    pb_push(SGL_CMD_BINDBUFFERBASE);
    pb_push(target);
    pb_push(index);
    pb_push(buffer);
}

void glClampColor(GLenum target, GLenum clamp)
{
    pb_push(SGL_CMD_CLAMPCOLOR);
    pb_push(target);
    pb_push(clamp);
}

void glBeginConditionalRender(GLuint id, GLenum mode)
{
    pb_push(SGL_CMD_BEGINCONDITIONALRENDER);
    pb_push(id);
    pb_push(mode);
}

void glEndConditionalRender(void)
{
    pb_push(SGL_CMD_ENDCONDITIONALRENDER);
}

void glVertexAttribI1i(GLuint index, GLint x)
{
    pb_push(SGL_CMD_VERTEXATTRIBI1I);
    pb_push(index);
    pb_push(x);
}

void glVertexAttribI2i(GLuint index, GLint x, GLint y)
{
    pb_push(SGL_CMD_VERTEXATTRIBI2I);
    pb_push(index);
    pb_push(x);
    pb_push(y);
}

void glVertexAttribI3i(GLuint index, GLint x, GLint y, GLint z)
{
    pb_push(SGL_CMD_VERTEXATTRIBI3I);
    pb_push(index);
    pb_push(x);
    pb_push(y);
    pb_push(z);
}

void glVertexAttribI4i(GLuint index, GLint x, GLint y, GLint z, GLint w)
{
    pb_push(SGL_CMD_VERTEXATTRIBI4I);
    pb_push(index);
    pb_push(x);
    pb_push(y);
    pb_push(z);
    pb_push(w);
}

void glVertexAttribI1ui(GLuint index, GLuint x)
{
    pb_push(SGL_CMD_VERTEXATTRIBI1UI);
    pb_push(index);
    pb_push(x);
}

void glVertexAttribI2ui(GLuint index, GLuint x, GLuint y)
{
    pb_push(SGL_CMD_VERTEXATTRIBI2UI);
    pb_push(index);
    pb_push(x);
    pb_push(y);
}

void glVertexAttribI3ui(GLuint index, GLuint x, GLuint y, GLuint z)
{
    pb_push(SGL_CMD_VERTEXATTRIBI3UI);
    pb_push(index);
    pb_push(x);
    pb_push(y);
    pb_push(z);
}

void glVertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w)
{
    pb_push(SGL_CMD_VERTEXATTRIBI4UI);
    pb_push(index);
    pb_push(x);
    pb_push(y);
    pb_push(z);
    pb_push(w);
}

void glUniform1ui(GLint location, GLuint v0)
{
    pb_push(SGL_CMD_UNIFORM1UI);
    pb_push(location);
    pb_push(v0);
}

void glUniform2ui(GLint location, GLuint v0, GLuint v1)
{
    pb_push(SGL_CMD_UNIFORM2UI);
    pb_push(location);
    pb_push(v0);
    pb_push(v1);
}

void glUniform3ui(GLint location, GLuint v0, GLuint v1, GLuint v2)
{
    pb_push(SGL_CMD_UNIFORM3UI);
    pb_push(location);
    pb_push(v0);
    pb_push(v1);
    pb_push(v2);
}

void glUniform4ui(GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3)
{
    pb_push(SGL_CMD_UNIFORM4UI);
    pb_push(location);
    pb_push(v0);
    pb_push(v1);
    pb_push(v2);
    pb_push(v3);
}

void glClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil)
{
    pb_push(SGL_CMD_CLEARBUFFERFI);
    pb_push(buffer);
    pb_push(drawbuffer);
    pb_pushf(depth);
    pb_push(stencil);
}

GLboolean glIsRenderbuffer(GLuint renderbuffer)
{
    pb_push(SGL_CMD_ISRENDERBUFFER);
    pb_push(renderbuffer);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glBindRenderbuffer(GLenum target, GLuint renderbuffer)
{
    pb_push(SGL_CMD_BINDRENDERBUFFER);
    pb_push(target);
    pb_push(renderbuffer);
}

void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height)
{
    pb_push(SGL_CMD_RENDERBUFFERSTORAGE);
    pb_push(target);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
}

GLboolean glIsFramebuffer(GLuint framebuffer)
{
    pb_push(SGL_CMD_ISFRAMEBUFFER);
    pb_push(framebuffer);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    pb_push(SGL_CMD_BINDFRAMEBUFFER);
    pb_push(target);
    pb_push(framebuffer);
}

GLenum glCheckFramebufferStatus(GLenum target)
{
    pb_push(SGL_CMD_CHECKFRAMEBUFFERSTATUS);
    pb_push(target);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glFramebufferTexture1D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
    pb_push(SGL_CMD_FRAMEBUFFERTEXTURE1D);
    pb_push(target);
    pb_push(attachment);
    pb_push(textarget);
    pb_push(texture);
    pb_push(level);
}

void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
    pb_push(SGL_CMD_FRAMEBUFFERTEXTURE2D);
    pb_push(target);
    pb_push(attachment);
    pb_push(textarget);
    pb_push(texture);
    pb_push(level);
}

void glFramebufferTexture3D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLint zoffset)
{
    pb_push(SGL_CMD_FRAMEBUFFERTEXTURE3D);
    pb_push(target);
    pb_push(attachment);
    pb_push(textarget);
    pb_push(texture);
    pb_push(level);
    pb_push(zoffset);
}

void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)
{
    pb_push(SGL_CMD_FRAMEBUFFERRENDERBUFFER);
    pb_push(target);
    pb_push(attachment);
    pb_push(renderbuffertarget);
    pb_push(renderbuffer);
}

void glGenerateMipmap(GLenum target)
{
    pb_push(SGL_CMD_GENERATEMIPMAP);
    pb_push(target);
}

void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter)
{
    pb_push(SGL_CMD_BLITFRAMEBUFFER);
    pb_push(srcX0);
    pb_push(srcY0);
    pb_push(srcX1);
    pb_push(srcY1);
    pb_push(dstX0);
    pb_push(dstY0);
    pb_push(dstX1);
    pb_push(dstY1);
    pb_push(mask);
    pb_push(filter);
}

void glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height)
{
    pb_push(SGL_CMD_RENDERBUFFERSTORAGEMULTISAMPLE);
    pb_push(target);
    pb_push(samples);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
}

void glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer)
{
    pb_push(SGL_CMD_FRAMEBUFFERTEXTURELAYER);
    pb_push(target);
    pb_push(attachment);
    pb_push(texture);
    pb_push(level);
    pb_push(layer);
}

void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length)
{
    glimpl_flush_mapped_buffer_range(SGL_CMD_FLUSHMAPPEDBUFFERRANGE, target, offset, length);
}

GLboolean glIsVertexArray(GLuint array)
{
    pb_push(SGL_CMD_ISVERTEXARRAY);
    pb_push(array);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount)
{
    pb_push(SGL_CMD_DRAWARRAYSINSTANCED);
    pb_push(mode);
    pb_push(first);
    pb_push(count);
    pb_push(instancecount);
}

void glTexBuffer(GLenum target, GLenum internalformat, GLuint buffer)
{
    pb_push(SGL_CMD_TEXBUFFER);
    pb_push(target);
    pb_push(internalformat);
    pb_push(buffer);
}

void glPrimitiveRestartIndex(GLuint index)
{
    pb_push(SGL_CMD_PRIMITIVERESTARTINDEX);
    pb_push(index);
}

void glCopyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size)
{
    pb_push(SGL_CMD_COPYBUFFERSUBDATA);
    pb_push(readTarget);
    pb_push(writeTarget);
    pb_push(readOffset);
    pb_push(writeOffset);
    pb_push(size);
}

void glUniformBlockBinding(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding)
{
    pb_push(SGL_CMD_UNIFORMBLOCKBINDING);
    pb_push(program);
    pb_push(uniformBlockIndex);
    pb_push(uniformBlockBinding);
}

void glProvokingVertex(GLenum mode)
{
    pb_push(SGL_CMD_PROVOKINGVERTEX);
    pb_push(mode);
}

void glFramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level)
{
    pb_push(SGL_CMD_FRAMEBUFFERTEXTURE);
    pb_push(target);
    pb_push(attachment);
    pb_push(texture);
    pb_push(level);
}

void glSampleMaski(GLuint maskNumber, GLbitfield mask)
{
    pb_push(SGL_CMD_SAMPLEMASKI);
    pb_push(maskNumber);
    pb_push(mask);
}

GLboolean glIsSampler(GLuint sampler)
{
    pb_push(SGL_CMD_ISSAMPLER);
    pb_push(sampler);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glBindSampler(GLuint unit, GLuint sampler)
{
    pb_push(SGL_CMD_BINDSAMPLER);
    pb_push(unit);
    pb_push(sampler);
}

void glSamplerParameteri(GLuint sampler, GLenum pname, GLint param)
{
    pb_push(SGL_CMD_SAMPLERPARAMETERI);
    pb_push(sampler);
    pb_push(pname);
    pb_push(param);
}

void glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param)
{
    pb_push(SGL_CMD_SAMPLERPARAMETERF);
    pb_push(sampler);
    pb_push(pname);
    pb_pushf(param);
}

void glQueryCounter(GLuint id, GLenum target)
{
    pb_push(SGL_CMD_QUERYCOUNTER);
    pb_push(id);
    pb_push(target);
}

void glVertexAttribDivisor(GLuint index, GLuint divisor)
{
    pb_push(SGL_CMD_VERTEXATTRIBDIVISOR);
    pb_push(index);
    pb_push(divisor);
}

void glVertexAttribP1ui(GLuint index, GLenum type, GLboolean normalized, GLuint value)
{
    pb_push(SGL_CMD_VERTEXATTRIBP1UI);
    pb_push(index);
    pb_push(type);
    pb_push(normalized);
    pb_push(value);
}

void glVertexAttribP2ui(GLuint index, GLenum type, GLboolean normalized, GLuint value)
{
    pb_push(SGL_CMD_VERTEXATTRIBP2UI);
    pb_push(index);
    pb_push(type);
    pb_push(normalized);
    pb_push(value);
}

void glVertexAttribP3ui(GLuint index, GLenum type, GLboolean normalized, GLuint value)
{
    pb_push(SGL_CMD_VERTEXATTRIBP3UI);
    pb_push(index);
    pb_push(type);
    pb_push(normalized);
    pb_push(value);
}

void glVertexAttribP4ui(GLuint index, GLenum type, GLboolean normalized, GLuint value)
{
    pb_push(SGL_CMD_VERTEXATTRIBP4UI);
    pb_push(index);
    pb_push(type);
    pb_push(normalized);
    pb_push(value);
}

void glVertexP2ui(GLenum type, GLuint value)
{
    pb_push(SGL_CMD_VERTEXP2UI);
    pb_push(type);
    pb_push(value);
}

void glVertexP3ui(GLenum type, GLuint value)
{
    pb_push(SGL_CMD_VERTEXP3UI);
    pb_push(type);
    pb_push(value);
}

void glVertexP4ui(GLenum type, GLuint value)
{
    pb_push(SGL_CMD_VERTEXP4UI);
    pb_push(type);
    pb_push(value);
}

void glTexCoordP1ui(GLenum type, GLuint coords)
{
    pb_push(SGL_CMD_TEXCOORDP1UI);
    pb_push(type);
    pb_push(coords);
}

void glTexCoordP2ui(GLenum type, GLuint coords)
{
    pb_push(SGL_CMD_TEXCOORDP2UI);
    pb_push(type);
    pb_push(coords);
}

void glTexCoordP3ui(GLenum type, GLuint coords)
{
    pb_push(SGL_CMD_TEXCOORDP3UI);
    pb_push(type);
    pb_push(coords);
}

void glTexCoordP4ui(GLenum type, GLuint coords)
{
    pb_push(SGL_CMD_TEXCOORDP4UI);
    pb_push(type);
    pb_push(coords);
}

void glMultiTexCoordP1ui(GLenum texture, GLenum type, GLuint coords)
{
    pb_push(SGL_CMD_MULTITEXCOORDP1UI);
    pb_push(texture);
    pb_push(type);
    pb_push(coords);
}

void glMultiTexCoordP2ui(GLenum texture, GLenum type, GLuint coords)
{
    pb_push(SGL_CMD_MULTITEXCOORDP2UI);
    pb_push(texture);
    pb_push(type);
    pb_push(coords);
}

void glMultiTexCoordP3ui(GLenum texture, GLenum type, GLuint coords)
{
    pb_push(SGL_CMD_MULTITEXCOORDP3UI);
    pb_push(texture);
    pb_push(type);
    pb_push(coords);
}

void glMultiTexCoordP4ui(GLenum texture, GLenum type, GLuint coords)
{
    pb_push(SGL_CMD_MULTITEXCOORDP4UI);
    pb_push(texture);
    pb_push(type);
    pb_push(coords);
}

void glNormalP3ui(GLenum type, GLuint coords)
{
    pb_push(SGL_CMD_NORMALP3UI);
    pb_push(type);
    pb_push(coords);
}

void glColorP3ui(GLenum type, GLuint color)
{
    pb_push(SGL_CMD_COLORP3UI);
    pb_push(type);
    pb_push(color);
}

void glColorP4ui(GLenum type, GLuint color)
{
    pb_push(SGL_CMD_COLORP4UI);
    pb_push(type);
    pb_push(color);
}

void glSecondaryColorP3ui(GLenum type, GLuint color)
{
    pb_push(SGL_CMD_SECONDARYCOLORP3UI);
    pb_push(type);
    pb_push(color);
}

void glMinSampleShading(GLfloat value)
{
    pb_push(SGL_CMD_MINSAMPLESHADING);
    pb_pushf(value);
}

void glBlendEquationi(GLuint buf, GLenum mode)
{
    pb_push(SGL_CMD_BLENDEQUATIONI);
    pb_push(buf);
    pb_push(mode);
}

void glBlendEquationSeparatei(GLuint buf, GLenum modeRGB, GLenum modeAlpha)
{
    pb_push(SGL_CMD_BLENDEQUATIONSEPARATEI);
    pb_push(buf);
    pb_push(modeRGB);
    pb_push(modeAlpha);
}

void glBlendFunci(GLuint buf, GLenum src, GLenum dst)
{
    pb_push(SGL_CMD_BLENDFUNCI);
    pb_push(buf);
    pb_push(src);
    pb_push(dst);
}

void glBlendFuncSeparatei(GLuint buf, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha)
{
    pb_push(SGL_CMD_BLENDFUNCSEPARATEI);
    pb_push(buf);
    pb_push(srcRGB);
    pb_push(dstRGB);
    pb_push(srcAlpha);
    pb_push(dstAlpha);
}

void glUniform1d(GLint location, GLdouble x)
{
    pb_push(SGL_CMD_UNIFORM1D);
    pb_push(location);
    pb_pushf(x);
}

void glUniform2d(GLint location, GLdouble x, GLdouble y)
{
    pb_push(SGL_CMD_UNIFORM2D);
    pb_push(location);
    pb_pushf(x);
    pb_pushf(y);
}

void glUniform3d(GLint location, GLdouble x, GLdouble y, GLdouble z)
{
    pb_push(SGL_CMD_UNIFORM3D);
    pb_push(location);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
}

void glUniform4d(GLint location, GLdouble x, GLdouble y, GLdouble z, GLdouble w)
{
    pb_push(SGL_CMD_UNIFORM4D);
    pb_push(location);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
    pb_pushf(w);
}

void glPatchParameteri(GLenum pname, GLint value)
{
    pb_push(SGL_CMD_PATCHPARAMETERI);
    pb_push(pname);
    pb_push(value);
}

void glBindTransformFeedback(GLenum target, GLuint id)
{
    pb_push(SGL_CMD_BINDTRANSFORMFEEDBACK);
    pb_push(target);
    pb_push(id);
}

GLboolean glIsTransformFeedback(GLuint id)
{
    pb_push(SGL_CMD_ISTRANSFORMFEEDBACK);
    pb_push(id);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glPauseTransformFeedback(void)
{
    pb_push(SGL_CMD_PAUSETRANSFORMFEEDBACK);
}

void glResumeTransformFeedback(void)
{
    pb_push(SGL_CMD_RESUMETRANSFORMFEEDBACK);
}

void glDrawTransformFeedback(GLenum mode, GLuint id)
{
    pb_push(SGL_CMD_DRAWTRANSFORMFEEDBACK);
    pb_push(mode);
    pb_push(id);
}

void glDrawTransformFeedbackStream(GLenum mode, GLuint id, GLuint stream)
{
    pb_push(SGL_CMD_DRAWTRANSFORMFEEDBACKSTREAM);
    pb_push(mode);
    pb_push(id);
    pb_push(stream);
}

void glBeginQueryIndexed(GLenum target, GLuint index, GLuint id)
{
    pb_push(SGL_CMD_BEGINQUERYINDEXED);
    pb_push(target);
    pb_push(index);
    pb_push(id);
}

void glEndQueryIndexed(GLenum target, GLuint index)
{
    pb_push(SGL_CMD_ENDQUERYINDEXED);
    pb_push(target);
    pb_push(index);
}

void glReleaseShaderCompiler(void)
{
    pb_push(SGL_CMD_RELEASESHADERCOMPILER);
}

void glDepthRangef(GLfloat n, GLfloat f)
{
    pb_push(SGL_CMD_DEPTHRANGEF);
    pb_pushf(n);
    pb_pushf(f);
}

void glClearDepthf(GLfloat d)
{
    pb_push(SGL_CMD_CLEARDEPTHF);
    pb_pushf(d);
}

void glProgramParameteri(GLuint program, GLenum pname, GLint value)
{
    pb_push(SGL_CMD_PROGRAMPARAMETERI);
    pb_push(program);
    pb_push(pname);
    pb_push(value);
}

void glUseProgramStages(GLuint pipeline, GLbitfield stages, GLuint program)
{
    pb_push(SGL_CMD_USEPROGRAMSTAGES);
    pb_push(pipeline);
    pb_push(stages);
    pb_push(program);
}

void glActiveShaderProgram(GLuint pipeline, GLuint program)
{
    pb_push(SGL_CMD_ACTIVESHADERPROGRAM);
    pb_push(pipeline);
    pb_push(program);
}

void glBindProgramPipeline(GLuint pipeline)
{
    pb_push(SGL_CMD_BINDPROGRAMPIPELINE);
    pb_push(pipeline);
}

GLboolean glIsProgramPipeline(GLuint pipeline)
{
    pb_push(SGL_CMD_ISPROGRAMPIPELINE);
    pb_push(pipeline);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glProgramUniform1i(GLuint program, GLint location, GLint v0)
{
    pb_push(SGL_CMD_PROGRAMUNIFORM1I);
    pb_push(program);
    pb_push(location);
    pb_push(v0);
}

void glProgramUniform1f(GLuint program, GLint location, GLfloat v0)
{
    pb_push(SGL_CMD_PROGRAMUNIFORM1F);
    pb_push(program);
    pb_push(location);
    pb_pushf(v0);
}

void glProgramUniform1d(GLuint program, GLint location, GLdouble v0)
{
    pb_push(SGL_CMD_PROGRAMUNIFORM1D);
    pb_push(program);
    pb_push(location);
    pb_pushf(v0);
}

void glProgramUniform1ui(GLuint program, GLint location, GLuint v0)
{
    pb_push(SGL_CMD_PROGRAMUNIFORM1UI);
    pb_push(program);
    pb_push(location);
    pb_push(v0);
}

void glProgramUniform2i(GLuint program, GLint location, GLint v0, GLint v1)
{
    pb_push(SGL_CMD_PROGRAMUNIFORM2I);
    pb_push(program);
    pb_push(location);
    pb_push(v0);
    pb_push(v1);
}

void glProgramUniform2f(GLuint program, GLint location, GLfloat v0, GLfloat v1)
{
    pb_push(SGL_CMD_PROGRAMUNIFORM2F);
    pb_push(program);
    pb_push(location);
    pb_pushf(v0);
    pb_pushf(v1);
}

void glProgramUniform2d(GLuint program, GLint location, GLdouble v0, GLdouble v1)
{
    pb_push(SGL_CMD_PROGRAMUNIFORM2D);
    pb_push(program);
    pb_push(location);
    pb_pushf(v0);
    pb_pushf(v1);
}

void glProgramUniform2ui(GLuint program, GLint location, GLuint v0, GLuint v1)
{
    pb_push(SGL_CMD_PROGRAMUNIFORM2UI);
    pb_push(program);
    pb_push(location);
    pb_push(v0);
    pb_push(v1);
}

void glProgramUniform3i(GLuint program, GLint location, GLint v0, GLint v1, GLint v2)
{
    pb_push(SGL_CMD_PROGRAMUNIFORM3I);
    pb_push(program);
    pb_push(location);
    pb_push(v0);
    pb_push(v1);
    pb_push(v2);
}

void glProgramUniform3f(GLuint program, GLint location, GLfloat v0, GLfloat v1, GLfloat v2)
{
    pb_push(SGL_CMD_PROGRAMUNIFORM3F);
    pb_push(program);
    pb_push(location);
    pb_pushf(v0);
    pb_pushf(v1);
    pb_pushf(v2);
}

void glProgramUniform3d(GLuint program, GLint location, GLdouble v0, GLdouble v1, GLdouble v2)
{
    pb_push(SGL_CMD_PROGRAMUNIFORM3D);
    pb_push(program);
    pb_push(location);
    pb_pushf(v0);
    pb_pushf(v1);
    pb_pushf(v2);
}

void glProgramUniform3ui(GLuint program, GLint location, GLuint v0, GLuint v1, GLuint v2)
{
    pb_push(SGL_CMD_PROGRAMUNIFORM3UI);
    pb_push(program);
    pb_push(location);
    pb_push(v0);
    pb_push(v1);
    pb_push(v2);
}

void glProgramUniform4i(GLuint program, GLint location, GLint v0, GLint v1, GLint v2, GLint v3)
{
    pb_push(SGL_CMD_PROGRAMUNIFORM4I);
    pb_push(program);
    pb_push(location);
    pb_push(v0);
    pb_push(v1);
    pb_push(v2);
    pb_push(v3);
}

void glProgramUniform4f(GLuint program, GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)
{
    pb_push(SGL_CMD_PROGRAMUNIFORM4F);
    pb_push(program);
    pb_push(location);
    pb_pushf(v0);
    pb_pushf(v1);
    pb_pushf(v2);
    pb_pushf(v3);
}

void glProgramUniform4d(GLuint program, GLint location, GLdouble v0, GLdouble v1, GLdouble v2, GLdouble v3)
{
    pb_push(SGL_CMD_PROGRAMUNIFORM4D);
    pb_push(program);
    pb_push(location);
    pb_pushf(v0);
    pb_pushf(v1);
    pb_pushf(v2);
    pb_pushf(v3);
}

void glProgramUniform4ui(GLuint program, GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3)
{
    pb_push(SGL_CMD_PROGRAMUNIFORM4UI);
    pb_push(program);
    pb_push(location);
    pb_push(v0);
    pb_push(v1);
    pb_push(v2);
    pb_push(v3);
}

void glValidateProgramPipeline(GLuint pipeline)
{
    pb_push(SGL_CMD_VALIDATEPROGRAMPIPELINE);
    pb_push(pipeline);
}

void glVertexAttribL1d(GLuint index, GLdouble x)
{
    pb_push(SGL_CMD_VERTEXATTRIBL1D);
    pb_push(index);
    pb_pushf(x);
}

void glVertexAttribL2d(GLuint index, GLdouble x, GLdouble y)
{
    pb_push(SGL_CMD_VERTEXATTRIBL2D);
    pb_push(index);
    pb_pushf(x);
    pb_pushf(y);
}

void glVertexAttribL3d(GLuint index, GLdouble x, GLdouble y, GLdouble z)
{
    pb_push(SGL_CMD_VERTEXATTRIBL3D);
    pb_push(index);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
}

void glVertexAttribL4d(GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w)
{
    pb_push(SGL_CMD_VERTEXATTRIBL4D);
    pb_push(index);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(z);
    pb_pushf(w);
}

void glViewportIndexedf(GLuint index, GLfloat x, GLfloat y, GLfloat w, GLfloat h)
{
    pb_push(SGL_CMD_VIEWPORTINDEXEDF);
    pb_push(index);
    pb_pushf(x);
    pb_pushf(y);
    pb_pushf(w);
    pb_pushf(h);
}

void glScissorIndexed(GLuint index, GLint left, GLint bottom, GLsizei width, GLsizei height)
{
    pb_push(SGL_CMD_SCISSORINDEXED);
    pb_push(index);
    pb_push(left);
    pb_push(bottom);
    pb_push(width);
    pb_push(height);
}

void glDepthRangeIndexed(GLuint index, GLdouble n, GLdouble f)
{
    pb_push(SGL_CMD_DEPTHRANGEINDEXED);
    pb_push(index);
    pb_pushf(n);
    pb_pushf(f);
}

void glDrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount, GLuint baseinstance)
{
    pb_push(SGL_CMD_DRAWARRAYSINSTANCEDBASEINSTANCE);
    pb_push(mode);
    pb_push(first);
    pb_push(count);
    pb_push(instancecount);
    pb_push(baseinstance);
}

void glBindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access, GLenum format)
{
    pb_push(SGL_CMD_BINDIMAGETEXTURE);
    pb_push(unit);
    pb_push(texture);
    pb_push(level);
    pb_push(layered);
    pb_push(layer);
    pb_push(access);
    pb_push(format);
}

void glMemoryBarrier(GLbitfield barriers)
{
    pb_push(SGL_CMD_MEMORYBARRIER);
    pb_push(barriers);
}

void glTexStorage1D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width)
{
    pb_push(SGL_CMD_TEXSTORAGE1D);
    pb_push(target);
    pb_push(levels);
    pb_push(internalformat);
    pb_push(width);
}

void glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height)
{
    pb_push(SGL_CMD_TEXSTORAGE2D);
    pb_push(target);
    pb_push(levels);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
}

void glTexStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth)
{
    pb_push(SGL_CMD_TEXSTORAGE3D);
    pb_push(target);
    pb_push(levels);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
    pb_push(depth);
}

void glDrawTransformFeedbackInstanced(GLenum mode, GLuint id, GLsizei instancecount)
{
    pb_push(SGL_CMD_DRAWTRANSFORMFEEDBACKINSTANCED);
    pb_push(mode);
    pb_push(id);
    pb_push(instancecount);
}

void glDrawTransformFeedbackStreamInstanced(GLenum mode, GLuint id, GLuint stream, GLsizei instancecount)
{
    pb_push(SGL_CMD_DRAWTRANSFORMFEEDBACKSTREAMINSTANCED);
    pb_push(mode);
    pb_push(id);
    pb_push(stream);
    pb_push(instancecount);
}

void glDispatchComputeIndirect(GLintptr indirect)
{
    pb_push(SGL_CMD_DISPATCHCOMPUTEINDIRECT);
    pb_push(indirect);
}

void glCopyImageSubData(GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ, GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth)
{
    pb_push(SGL_CMD_COPYIMAGESUBDATA);
    pb_push(srcName);
    pb_push(srcTarget);
    pb_push(srcLevel);
    pb_push(srcX);
    pb_push(srcY);
    pb_push(srcZ);
    pb_push(dstName);
    pb_push(dstTarget);
    pb_push(dstLevel);
    pb_push(dstX);
    pb_push(dstY);
    pb_push(dstZ);
    pb_push(srcWidth);
    pb_push(srcHeight);
    pb_push(srcDepth);
}

void glFramebufferParameteri(GLenum target, GLenum pname, GLint param)
{
    pb_push(SGL_CMD_FRAMEBUFFERPARAMETERI);
    pb_push(target);
    pb_push(pname);
    pb_push(param);
}

void glInvalidateTexSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth)
{
    pb_push(SGL_CMD_INVALIDATETEXSUBIMAGE);
    pb_push(texture);
    pb_push(level);
    pb_push(xoffset);
    pb_push(yoffset);
    pb_push(zoffset);
    pb_push(width);
    pb_push(height);
    pb_push(depth);
}

void glInvalidateTexImage(GLuint texture, GLint level)
{
    pb_push(SGL_CMD_INVALIDATETEXIMAGE);
    pb_push(texture);
    pb_push(level);
}

void glInvalidateBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr length)
{
    pb_push(SGL_CMD_INVALIDATEBUFFERSUBDATA);
    pb_push(buffer);
    pb_push(offset);
    pb_push(length);
}

void glInvalidateBufferData(GLuint buffer)
{
    pb_push(SGL_CMD_INVALIDATEBUFFERDATA);
    pb_push(buffer);
}

void glShaderStorageBlockBinding(GLuint program, GLuint storageBlockIndex, GLuint storageBlockBinding)
{
    pb_push(SGL_CMD_SHADERSTORAGEBLOCKBINDING);
    pb_push(program);
    pb_push(storageBlockIndex);
    pb_push(storageBlockBinding);
}

void glTexBufferRange(GLenum target, GLenum internalformat, GLuint buffer, GLintptr offset, GLsizeiptr size)
{
    pb_push(SGL_CMD_TEXBUFFERRANGE);
    pb_push(target);
    pb_push(internalformat);
    pb_push(buffer);
    pb_push(offset);
    pb_push(size);
}

void glTexStorage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations)
{
    pb_push(SGL_CMD_TEXSTORAGE2DMULTISAMPLE);
    pb_push(target);
    pb_push(samples);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
    pb_push(fixedsamplelocations);
}

void glTexStorage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedsamplelocations)
{
    pb_push(SGL_CMD_TEXSTORAGE3DMULTISAMPLE);
    pb_push(target);
    pb_push(samples);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
    pb_push(depth);
    pb_push(fixedsamplelocations);
}

void glTextureView(GLuint texture, GLenum target, GLuint origtexture, GLenum internalformat, GLuint minlevel, GLuint numlevels, GLuint minlayer, GLuint numlayers)
{
    pb_push(SGL_CMD_TEXTUREVIEW);
    pb_push(texture);
    pb_push(target);
    pb_push(origtexture);
    pb_push(internalformat);
    pb_push(minlevel);
    pb_push(numlevels);
    pb_push(minlayer);
    pb_push(numlayers);
}

void glBindVertexBuffer(GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride)
{
    pb_push(SGL_CMD_BINDVERTEXBUFFER);
    pb_push(bindingindex);
    pb_push(buffer);
    pb_push(offset);
    pb_push(stride);
}

void glVertexAttribFormat(GLuint attribindex, GLint size, GLenum type, GLboolean normalized, GLuint relativeoffset)
{
    pb_push(SGL_CMD_VERTEXATTRIBFORMAT);
    pb_push(attribindex);
    pb_push(size);
    pb_push(type);
    pb_push(normalized);
    pb_push(relativeoffset);
}

void glVertexAttribIFormat(GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset)
{
    pb_push(SGL_CMD_VERTEXATTRIBIFORMAT);
    pb_push(attribindex);
    pb_push(size);
    pb_push(type);
    pb_push(relativeoffset);
}

void glVertexAttribLFormat(GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset)
{
    pb_push(SGL_CMD_VERTEXATTRIBLFORMAT);
    pb_push(attribindex);
    pb_push(size);
    pb_push(type);
    pb_push(relativeoffset);
}

void glVertexAttribBinding(GLuint attribindex, GLuint bindingindex)
{
    pb_push(SGL_CMD_VERTEXATTRIBBINDING);
    pb_push(attribindex);
    pb_push(bindingindex);
}

void glVertexBindingDivisor(GLuint bindingindex, GLuint divisor)
{
    pb_push(SGL_CMD_VERTEXBINDINGDIVISOR);
    pb_push(bindingindex);
    pb_push(divisor);
}

void glPopDebugGroup(void)
{
    pb_push(SGL_CMD_POPDEBUGGROUP);
}

void glClipControl(GLenum origin, GLenum depth)
{
    pb_push(SGL_CMD_CLIPCONTROL);
    pb_push(origin);
    pb_push(depth);
}

void glTransformFeedbackBufferBase(GLuint xfb, GLuint index, GLuint buffer)
{
    pb_push(SGL_CMD_TRANSFORMFEEDBACKBUFFERBASE);
    pb_push(xfb);
    pb_push(index);
    pb_push(buffer);
}

void glTransformFeedbackBufferRange(GLuint xfb, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size)
{
    pb_push(SGL_CMD_TRANSFORMFEEDBACKBUFFERRANGE);
    pb_push(xfb);
    pb_push(index);
    pb_push(buffer);
    pb_push(offset);
    pb_push(size);
}

void glCopyNamedBufferSubData(GLuint readBuffer, GLuint writeBuffer, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size)
{
    pb_push(SGL_CMD_COPYNAMEDBUFFERSUBDATA);
    pb_push(readBuffer);
    pb_push(writeBuffer);
    pb_push(readOffset);
    pb_push(writeOffset);
    pb_push(size);
}

GLboolean glUnmapNamedBuffer(GLuint buffer)
{
    return glimpl_unmap_buffer(SGL_CMD_UNMAPNAMEDBUFFER, buffer);
}

void glFlushMappedNamedBufferRange(GLuint buffer, GLintptr offset, GLsizeiptr length)
{
    glimpl_flush_mapped_buffer_range(SGL_CMD_FLUSHMAPPEDNAMEDBUFFERRANGE, buffer, offset, length);
}

void glNamedFramebufferRenderbuffer(GLuint framebuffer, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)
{
    pb_push(SGL_CMD_NAMEDFRAMEBUFFERRENDERBUFFER);
    pb_push(framebuffer);
    pb_push(attachment);
    pb_push(renderbuffertarget);
    pb_push(renderbuffer);
}

void glNamedFramebufferParameteri(GLuint framebuffer, GLenum pname, GLint param)
{
    pb_push(SGL_CMD_NAMEDFRAMEBUFFERPARAMETERI);
    pb_push(framebuffer);
    pb_push(pname);
    pb_push(param);
}

void glNamedFramebufferTexture(GLuint framebuffer, GLenum attachment, GLuint texture, GLint level)
{
    pb_push(SGL_CMD_NAMEDFRAMEBUFFERTEXTURE);
    pb_push(framebuffer);
    pb_push(attachment);
    pb_push(texture);
    pb_push(level);
}

void glNamedFramebufferTextureLayer(GLuint framebuffer, GLenum attachment, GLuint texture, GLint level, GLint layer)
{
    pb_push(SGL_CMD_NAMEDFRAMEBUFFERTEXTURELAYER);
    pb_push(framebuffer);
    pb_push(attachment);
    pb_push(texture);
    pb_push(level);
    pb_push(layer);
}

void glNamedFramebufferDrawBuffer(GLuint framebuffer, GLenum buf)
{
    pb_push(SGL_CMD_NAMEDFRAMEBUFFERDRAWBUFFER);
    pb_push(framebuffer);
    pb_push(buf);
}

void glNamedFramebufferReadBuffer(GLuint framebuffer, GLenum src)
{
    pb_push(SGL_CMD_NAMEDFRAMEBUFFERREADBUFFER);
    pb_push(framebuffer);
    pb_push(src);
}

void glClearNamedFramebufferfi(GLuint framebuffer, GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil)
{
    pb_push(SGL_CMD_CLEARNAMEDFRAMEBUFFERFI);
    pb_push(framebuffer);
    pb_push(buffer);
    pb_push(drawbuffer);
    pb_pushf(depth);
    pb_push(stencil);
}

void glBlitNamedFramebuffer(GLuint readFramebuffer, GLuint drawFramebuffer, GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter)
{
    pb_push(SGL_CMD_BLITNAMEDFRAMEBUFFER);
    pb_push(readFramebuffer);
    pb_push(drawFramebuffer);
    pb_push(srcX0);
    pb_push(srcY0);
    pb_push(srcX1);
    pb_push(srcY1);
    pb_push(dstX0);
    pb_push(dstY0);
    pb_push(dstX1);
    pb_push(dstY1);
    pb_push(mask);
    pb_push(filter);
}

GLenum glCheckNamedFramebufferStatus(GLuint framebuffer, GLenum target)
{
    pb_push(SGL_CMD_CHECKNAMEDFRAMEBUFFERSTATUS);
    pb_push(framebuffer);
    pb_push(target);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glNamedRenderbufferStorage(GLuint renderbuffer, GLenum internalformat, GLsizei width, GLsizei height)
{
    pb_push(SGL_CMD_NAMEDRENDERBUFFERSTORAGE);
    pb_push(renderbuffer);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
}

void glNamedRenderbufferStorageMultisample(GLuint renderbuffer, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height)
{
    pb_push(SGL_CMD_NAMEDRENDERBUFFERSTORAGEMULTISAMPLE);
    pb_push(renderbuffer);
    pb_push(samples);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
}

void glTextureBuffer(GLuint texture, GLenum internalformat, GLuint buffer)
{
    pb_push(SGL_CMD_TEXTUREBUFFER);
    pb_push(texture);
    pb_push(internalformat);
    pb_push(buffer);
}

void glTextureBufferRange(GLuint texture, GLenum internalformat, GLuint buffer, GLintptr offset, GLsizeiptr size)
{
    pb_push(SGL_CMD_TEXTUREBUFFERRANGE);
    pb_push(texture);
    pb_push(internalformat);
    pb_push(buffer);
    pb_push(offset);
    pb_push(size);
}

void glTextureStorage1D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width)
{
    pb_push(SGL_CMD_TEXTURESTORAGE1D);
    pb_push(texture);
    pb_push(levels);
    pb_push(internalformat);
    pb_push(width);
}

void glTextureStorage2D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height)
{
    pb_push(SGL_CMD_TEXTURESTORAGE2D);
    pb_push(texture);
    pb_push(levels);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
}

void glTextureStorage3D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth)
{
    pb_push(SGL_CMD_TEXTURESTORAGE3D);
    pb_push(texture);
    pb_push(levels);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
    pb_push(depth);
}

void glTextureStorage2DMultisample(GLuint texture, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations)
{
    pb_push(SGL_CMD_TEXTURESTORAGE2DMULTISAMPLE);
    pb_push(texture);
    pb_push(samples);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
    pb_push(fixedsamplelocations);
}

void glTextureStorage3DMultisample(GLuint texture, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedsamplelocations)
{
    pb_push(SGL_CMD_TEXTURESTORAGE3DMULTISAMPLE);
    pb_push(texture);
    pb_push(samples);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
    pb_push(depth);
    pb_push(fixedsamplelocations);
}

void glCopyTextureSubImage1D(GLuint texture, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width)
{
    pb_push(SGL_CMD_COPYTEXTURESUBIMAGE1D);
    pb_push(texture);
    pb_push(level);
    pb_push(xoffset);
    pb_push(x);
    pb_push(y);
    pb_push(width);
}

void glCopyTextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
    pb_push(SGL_CMD_COPYTEXTURESUBIMAGE2D);
    pb_push(texture);
    pb_push(level);
    pb_push(xoffset);
    pb_push(yoffset);
    pb_push(x);
    pb_push(y);
    pb_push(width);
    pb_push(height);
}

void glCopyTextureSubImage3D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
    pb_push(SGL_CMD_COPYTEXTURESUBIMAGE3D);
    pb_push(texture);
    pb_push(level);
    pb_push(xoffset);
    pb_push(yoffset);
    pb_push(zoffset);
    pb_push(x);
    pb_push(y);
    pb_push(width);
    pb_push(height);
}

void glTextureParameterf(GLuint texture, GLenum pname, GLfloat param)
{
    pb_push(SGL_CMD_TEXTUREPARAMETERF);
    pb_push(texture);
    pb_push(pname);
    pb_pushf(param);
}

void glTextureParameteri(GLuint texture, GLenum pname, GLint param)
{
    pb_push(SGL_CMD_TEXTUREPARAMETERI);
    pb_push(texture);
    pb_push(pname);
    pb_push(param);
}

void glGenerateTextureMipmap(GLuint texture)
{
    pb_push(SGL_CMD_GENERATETEXTUREMIPMAP);
    pb_push(texture);
}

void glBindTextureUnit(GLuint unit, GLuint texture)
{
    pb_push(SGL_CMD_BINDTEXTUREUNIT);
    pb_push(unit);
    pb_push(texture);
}

void glDisableVertexArrayAttrib(GLuint vaobj, GLuint index)
{
    pb_push(SGL_CMD_DISABLEVERTEXARRAYATTRIB);
    pb_push(vaobj);
    pb_push(index);
}

void glEnableVertexArrayAttrib(GLuint vaobj, GLuint index)
{
    pb_push(SGL_CMD_ENABLEVERTEXARRAYATTRIB);
    pb_push(vaobj);
    pb_push(index);
}

void glVertexArrayElementBuffer(GLuint vaobj, GLuint buffer)
{
    pb_push(SGL_CMD_VERTEXARRAYELEMENTBUFFER);
    pb_push(vaobj);
    pb_push(buffer);
}

void glVertexArrayVertexBuffer(GLuint vaobj, GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride)
{
    pb_push(SGL_CMD_VERTEXARRAYVERTEXBUFFER);
    pb_push(vaobj);
    pb_push(bindingindex);
    pb_push(buffer);
    pb_push(offset);
    pb_push(stride);
}

void glVertexArrayAttribBinding(GLuint vaobj, GLuint attribindex, GLuint bindingindex)
{
    pb_push(SGL_CMD_VERTEXARRAYATTRIBBINDING);
    pb_push(vaobj);
    pb_push(attribindex);
    pb_push(bindingindex);
}

void glVertexArrayAttribFormat(GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLboolean normalized, GLuint relativeoffset)
{
    pb_push(SGL_CMD_VERTEXARRAYATTRIBFORMAT);
    pb_push(vaobj);
    pb_push(attribindex);
    pb_push(size);
    pb_push(type);
    pb_push(normalized);
    pb_push(relativeoffset);
}

void glVertexArrayAttribIFormat(GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset)
{
    pb_push(SGL_CMD_VERTEXARRAYATTRIBIFORMAT);
    pb_push(vaobj);
    pb_push(attribindex);
    pb_push(size);
    pb_push(type);
    pb_push(relativeoffset);
}

void glVertexArrayAttribLFormat(GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset)
{
    pb_push(SGL_CMD_VERTEXARRAYATTRIBLFORMAT);
    pb_push(vaobj);
    pb_push(attribindex);
    pb_push(size);
    pb_push(type);
    pb_push(relativeoffset);
}

void glVertexArrayBindingDivisor(GLuint vaobj, GLuint bindingindex, GLuint divisor)
{
    pb_push(SGL_CMD_VERTEXARRAYBINDINGDIVISOR);
    pb_push(vaobj);
    pb_push(bindingindex);
    pb_push(divisor);
}

void glGetQueryBufferObjecti64v(GLuint id, GLuint buffer, GLenum pname, GLintptr offset)
{
    pb_push(SGL_CMD_GETQUERYBUFFEROBJECTI64V);
    pb_push(id);
    pb_push(buffer);
    pb_push(pname);
    pb_push(offset);
}

void glGetQueryBufferObjectiv(GLuint id, GLuint buffer, GLenum pname, GLintptr offset)
{
    pb_push(SGL_CMD_GETQUERYBUFFEROBJECTIV);
    pb_push(id);
    pb_push(buffer);
    pb_push(pname);
    pb_push(offset);
}

void glGetQueryBufferObjectui64v(GLuint id, GLuint buffer, GLenum pname, GLintptr offset)
{
    pb_push(SGL_CMD_GETQUERYBUFFEROBJECTUI64V);
    pb_push(id);
    pb_push(buffer);
    pb_push(pname);
    pb_push(offset);
}

void glGetQueryBufferObjectuiv(GLuint id, GLuint buffer, GLenum pname, GLintptr offset)
{
    pb_push(SGL_CMD_GETQUERYBUFFEROBJECTUIV);
    pb_push(id);
    pb_push(buffer);
    pb_push(pname);
    pb_push(offset);
}

void glMemoryBarrierByRegion(GLbitfield barriers)
{
    pb_push(SGL_CMD_MEMORYBARRIERBYREGION);
    pb_push(barriers);
}

GLenum glGetGraphicsResetStatus(void)
{
    pb_push(SGL_CMD_GETGRAPHICSRESETSTATUS);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glTextureBarrier(void)
{
    pb_push(SGL_CMD_TEXTUREBARRIER);
}

void glPolygonOffsetClamp(GLfloat factor, GLfloat units, GLfloat clamp)
{
    pb_push(SGL_CMD_POLYGONOFFSETCLAMP);
    pb_pushf(factor);
    pb_pushf(units);
    pb_pushf(clamp);
}

void glColor3bv(const GLbyte* v)
{
    glColor3b(v[0], v[1], v[2]);
}

void glColor3dv(const GLdouble* v)
{
    glColor3d(v[0], v[1], v[2]);
}

void glColor3fv(const GLfloat* v)
{
    glColor3f(v[0], v[1], v[2]);
}

void glColor3iv(const GLint* v)
{
    glColor3i(v[0], v[1], v[2]);
}

void glColor3sv(const GLshort* v)
{
    glColor3s(v[0], v[1], v[2]);
}

void glColor3ubv(const GLubyte* v)
{
    glColor3ub(v[0], v[1], v[2]);
}

void glColor3uiv(const GLuint* v)
{
    glColor3ui(v[0], v[1], v[2]);
}

void glColor3usv(const GLushort* v)
{
    glColor3us(v[0], v[1], v[2]);
}

void glColor4bv(const GLbyte* v)
{
    glColor4b(v[0], v[1], v[2], v[3]);
}

void glColor4dv(const GLdouble* v)
{
    glColor4d(v[0], v[1], v[2], v[3]);
}

void glColor4iv(const GLint* v)
{
    glColor4i(v[0], v[1], v[2], v[3]);
}

void glColor4sv(const GLshort* v)
{
    glColor4s(v[0], v[1], v[2], v[3]);
}

void glColor4ubv(const GLubyte* v)
{
    glColor4ub(v[0], v[1], v[2], v[3]);
}

void glColor4uiv(const GLuint* v)
{
    glColor4ui(v[0], v[1], v[2], v[3]);
}

void glColor4usv(const GLushort* v)
{
    glColor4us(v[0], v[1], v[2], v[3]);
}

void glNormal3bv(const GLbyte* v)
{
    glNormal3b(v[0], v[1], v[2]);
}

void glNormal3dv(const GLdouble* v)
{
    glNormal3d(v[0], v[1], v[2]);
}

void glNormal3fv(const GLfloat* v)
{
    glNormal3f(v[0], v[1], v[2]);
}

void glNormal3iv(const GLint* v)
{
    glNormal3i(v[0], v[1], v[2]);
}

void glNormal3sv(const GLshort* v)
{
    glNormal3s(v[0], v[1], v[2]);
}

void glRasterPos2dv(const GLdouble* v)
{
    glRasterPos2d(v[0], v[1]);
}

void glRasterPos2fv(const GLfloat* v)
{
    glRasterPos2f(v[0], v[1]);
}

void glRasterPos2iv(const GLint* v)
{
    glRasterPos2i(v[0], v[1]);
}

void glRasterPos2sv(const GLshort* v)
{
    glRasterPos2s(v[0], v[1]);
}

void glRasterPos3dv(const GLdouble* v)
{
    glRasterPos3d(v[0], v[1], v[2]);
}

void glRasterPos3fv(const GLfloat* v)
{
    glRasterPos3f(v[0], v[1], v[2]);
}

void glRasterPos3iv(const GLint* v)
{
    glRasterPos3i(v[0], v[1], v[2]);
}

void glRasterPos3sv(const GLshort* v)
{
    glRasterPos3s(v[0], v[1], v[2]);
}

void glRasterPos4dv(const GLdouble* v)
{
    glRasterPos4d(v[0], v[1], v[2], v[3]);
}

void glRasterPos4fv(const GLfloat* v)
{
    glRasterPos4f(v[0], v[1], v[2], v[3]);
}

void glRasterPos4iv(const GLint* v)
{
    glRasterPos4i(v[0], v[1], v[2], v[3]);
}

void glRasterPos4sv(const GLshort* v)
{
    glRasterPos4s(v[0], v[1], v[2], v[3]);
}

void glTexCoord1dv(const GLdouble* v)
{
    glTexCoord1d(v[0]);
}

void glTexCoord1fv(const GLfloat* v)
{
    glTexCoord1f(v[0]);
}

void glTexCoord1iv(const GLint* v)
{
    glTexCoord1i(v[0]);
}

void glTexCoord1sv(const GLshort* v)
{
    glTexCoord1s(v[0]);
}

void glTexCoord2dv(const GLdouble* v)
{
    glTexCoord2d(v[0], v[1]);
}

void glTexCoord2fv(const GLfloat* v)
{
    glTexCoord2f(v[0], v[1]);
}

void glTexCoord2iv(const GLint* v)
{
    glTexCoord2i(v[0], v[1]);
}

void glTexCoord2sv(const GLshort* v)
{
    glTexCoord2s(v[0], v[1]);
}

void glTexCoord3dv(const GLdouble* v)
{
    glTexCoord3d(v[0], v[1], v[2]);
}

void glTexCoord3fv(const GLfloat* v)
{
    glTexCoord3f(v[0], v[1], v[2]);
}

void glTexCoord3iv(const GLint* v)
{
    glTexCoord3i(v[0], v[1], v[2]);
}

void glTexCoord3sv(const GLshort* v)
{
    glTexCoord3s(v[0], v[1], v[2]);
}

void glTexCoord4dv(const GLdouble* v)
{
    glTexCoord4d(v[0], v[1], v[2], v[3]);
}

void glTexCoord4fv(const GLfloat* v)
{
    glTexCoord4f(v[0], v[1], v[2], v[3]);
}

void glTexCoord4iv(const GLint* v)
{
    glTexCoord4i(v[0], v[1], v[2], v[3]);
}

void glTexCoord4sv(const GLshort* v)
{
    glTexCoord4s(v[0], v[1], v[2], v[3]);
}

void glVertex2dv(const GLdouble* v)
{
    glVertex2d(v[0], v[1]);
}

void glVertex2fv(const GLfloat* v)
{
    glVertex2f(v[0], v[1]);
}

void glVertex2iv(const GLint* v)
{
    glVertex2i(v[0], v[1]);
}

void glVertex2sv(const GLshort* v)
{
    glVertex2s(v[0], v[1]);
}

void glVertex3dv(const GLdouble* v)
{
    glVertex3d(v[0], v[1], v[2]);
}

void glVertex3fv(const GLfloat* v)
{
    glVertex3f(v[0], v[1], v[2]);
}

void glVertex3iv(const GLint* v)
{
    glVertex3i(v[0], v[1], v[2]);
}

void glVertex3sv(const GLshort* v)
{
    glVertex3s(v[0], v[1], v[2]);
}

void glVertex4dv(const GLdouble* v)
{
    glVertex4d(v[0], v[1], v[2], v[3]);
}

void glVertex4fv(const GLfloat* v)
{
    glVertex4f(v[0], v[1], v[2], v[3]);
}

void glVertex4iv(const GLint* v)
{
    glVertex4i(v[0], v[1], v[2], v[3]);
}

void glVertex4sv(const GLshort* v)
{
    glVertex4s(v[0], v[1], v[2], v[3]);
}

void glEvalCoord1dv(const GLdouble* u)
{
    glEvalCoord1d(u[0]);
}

void glEvalCoord1fv(const GLfloat* u)
{
    glEvalCoord1f(u[0]);
}

void glEvalCoord2dv(const GLdouble* u)
{
    glEvalCoord2d(u[0], u[1]);
}

void glEvalCoord2fv(const GLfloat* u)
{
    glEvalCoord2f(u[0], u[1]);
}

void glMultiTexCoord1dv(GLenum target, const GLdouble* v)
{
    glMultiTexCoord1d(target, v[0]);
}

void glMultiTexCoord1fv(GLenum target, const GLfloat* v)
{
    glMultiTexCoord1f(target, v[0]);
}

void glMultiTexCoord1iv(GLenum target, const GLint* v)
{
    glMultiTexCoord1i(target, v[0]);
}

void glMultiTexCoord1sv(GLenum target, const GLshort* v)
{
    glMultiTexCoord1s(target, v[0]);
}

void glMultiTexCoord2dv(GLenum target, const GLdouble* v)
{
    glMultiTexCoord2d(target, v[0], v[1]);
}

void glMultiTexCoord2fv(GLenum target, const GLfloat* v)
{
    glMultiTexCoord2f(target, v[0], v[1]);
}

void glMultiTexCoord2iv(GLenum target, const GLint* v)
{
    glMultiTexCoord2i(target, v[0], v[1]);
}

void glMultiTexCoord2sv(GLenum target, const GLshort* v)
{
    glMultiTexCoord2s(target, v[0], v[1]);
}

void glMultiTexCoord3dv(GLenum target, const GLdouble* v)
{
    glMultiTexCoord3d(target, v[0], v[1], v[2]);
}

void glMultiTexCoord3fv(GLenum target, const GLfloat* v)
{
    glMultiTexCoord3f(target, v[0], v[1], v[2]);
}

void glMultiTexCoord3iv(GLenum target, const GLint* v)
{
    glMultiTexCoord3i(target, v[0], v[1], v[2]);
}

void glMultiTexCoord3sv(GLenum target, const GLshort* v)
{
    glMultiTexCoord3s(target, v[0], v[1], v[2]);
}

void glMultiTexCoord4dv(GLenum target, const GLdouble* v)
{
    glMultiTexCoord4d(target, v[0], v[1], v[2], v[3]);
}

void glMultiTexCoord4fv(GLenum target, const GLfloat* v)
{
    glMultiTexCoord4f(target, v[0], v[1], v[2], v[3]);
}

void glMultiTexCoord4iv(GLenum target, const GLint* v)
{
    glMultiTexCoord4i(target, v[0], v[1], v[2], v[3]);
}

void glMultiTexCoord4sv(GLenum target, const GLshort* v)
{
    glMultiTexCoord4s(target, v[0], v[1], v[2], v[3]);
}

void glSecondaryColor3bv(const GLbyte* v)
{
    glSecondaryColor3b(v[0], v[1], v[2]);
}

void glSecondaryColor3dv(const GLdouble* v)
{
    glSecondaryColor3d(v[0], v[1], v[2]);
}

void glSecondaryColor3fv(const GLfloat* v)
{
    glSecondaryColor3f(v[0], v[1], v[2]);
}

void glSecondaryColor3iv(const GLint* v)
{
    glSecondaryColor3i(v[0], v[1], v[2]);
}

void glSecondaryColor3sv(const GLshort* v)
{
    glSecondaryColor3s(v[0], v[1], v[2]);
}

void glSecondaryColor3ubv(const GLubyte* v)
{
    glSecondaryColor3ub(v[0], v[1], v[2]);
}

void glSecondaryColor3uiv(const GLuint* v)
{
    glSecondaryColor3ui(v[0], v[1], v[2]);
}

void glSecondaryColor3usv(const GLushort* v)
{
    glSecondaryColor3us(v[0], v[1], v[2]);
}

void glWindowPos2dv(const GLdouble* v)
{
    glWindowPos2d(v[0], v[1]);
}

void glWindowPos2fv(const GLfloat* v)
{
    glWindowPos2f(v[0], v[1]);
}

void glWindowPos2iv(const GLint* v)
{
    glWindowPos2i(v[0], v[1]);
}

void glWindowPos2sv(const GLshort* v)
{
    glWindowPos2s(v[0], v[1]);
}

void glWindowPos3dv(const GLdouble* v)
{
    glWindowPos3d(v[0], v[1], v[2]);
}

void glWindowPos3fv(const GLfloat* v)
{
    glWindowPos3f(v[0], v[1], v[2]);
}

void glWindowPos3iv(const GLint* v)
{
    glWindowPos3i(v[0], v[1], v[2]);
}

void glWindowPos3sv(const GLshort* v)
{
    glWindowPos3s(v[0], v[1], v[2]);
}

void glUniform1fv(GLint location, GLsizei count, const GLfloat* value)
{
    for (int i = 0; i < count; i++)
        glUniform1f(location, value[i * 1 + 0]);
}

void glUniform2fv(GLint location, GLsizei count, const GLfloat* value)
{
    for (int i = 0; i < count; i++)
        glUniform2f(location, value[i * 2 + 0], value[i * 2 + 1]);
}

void glUniform3fv(GLint location, GLsizei count, const GLfloat* value)
{
    for (int i = 0; i < count; i++)
        glUniform3f(location, value[i * 3 + 0], value[i * 3 + 1], value[i * 3 + 2]);
}

void glUniform4fv(GLint location, GLsizei count, const GLfloat* value)
{
    for (int i = 0; i < count; i++)
        glUniform4f(location, value[i * 4 + 0], value[i * 4 + 1], value[i * 4 + 2], value[i * 4 + 3]);
}

void glUniform1iv(GLint location, GLsizei count, const GLint* value)
{
    for (int i = 0; i < count; i++)
        glUniform1i(location, value[i * 1 + 0]);
}

void glUniform2iv(GLint location, GLsizei count, const GLint* value)
{
    for (int i = 0; i < count; i++)
        glUniform2i(location, value[i * 2 + 0], value[i * 2 + 1]);
}

void glUniform3iv(GLint location, GLsizei count, const GLint* value)
{
    for (int i = 0; i < count; i++)
        glUniform3i(location, value[i * 3 + 0], value[i * 3 + 1], value[i * 3 + 2]);
}

void glUniform4iv(GLint location, GLsizei count, const GLint* value)
{
    for (int i = 0; i < count; i++)
        glUniform4i(location, value[i * 4 + 0], value[i * 4 + 1], value[i * 4 + 2], value[i * 4 + 3]);
}

void glVertexAttrib1dv(GLuint index, const GLdouble* v)
{
    glVertexAttrib1d(index, v[0]);
}

void glVertexAttrib1fv(GLuint index, const GLfloat* v)
{
    glVertexAttrib1f(index, v[0]);
}

void glVertexAttrib1sv(GLuint index, const GLshort* v)
{
    glVertexAttrib1s(index, v[0]);
}

void glVertexAttrib2dv(GLuint index, const GLdouble* v)
{
    glVertexAttrib2d(index, v[0], v[1]);
}

void glVertexAttrib2fv(GLuint index, const GLfloat* v)
{
    glVertexAttrib2f(index, v[0], v[1]);
}

void glVertexAttrib2sv(GLuint index, const GLshort* v)
{
    glVertexAttrib2s(index, v[0], v[1]);
}

void glVertexAttrib3dv(GLuint index, const GLdouble* v)
{
    glVertexAttrib3d(index, v[0], v[1], v[2]);
}

void glVertexAttrib3fv(GLuint index, const GLfloat* v)
{
    glVertexAttrib3f(index, v[0], v[1], v[2]);
}

void glVertexAttrib3sv(GLuint index, const GLshort* v)
{
    glVertexAttrib3s(index, v[0], v[1], v[2]);
}

void glVertexAttrib4Nubv(GLuint index, const GLubyte* v)
{
    glVertexAttrib4Nub(index, v[0], v[1], v[2], v[3]);
}

void glVertexAttrib4dv(GLuint index, const GLdouble* v)
{
    glVertexAttrib4d(index, v[0], v[1], v[2], v[3]);
}

void glVertexAttrib4fv(GLuint index, const GLfloat* v)
{
    glVertexAttrib4f(index, v[0], v[1], v[2], v[3]);
}

void glVertexAttrib4sv(GLuint index, const GLshort* v)
{
    glVertexAttrib4s(index, v[0], v[1], v[2], v[3]);
}

void glVertexAttribI1iv(GLuint index, const GLint* v)
{
    glVertexAttribI1i(index, v[0]);
}

void glVertexAttribI2iv(GLuint index, const GLint* v)
{
    glVertexAttribI2i(index, v[0], v[1]);
}

void glVertexAttribI3iv(GLuint index, const GLint* v)
{
    glVertexAttribI3i(index, v[0], v[1], v[2]);
}

void glVertexAttribI4iv(GLuint index, const GLint* v)
{
    glVertexAttribI4i(index, v[0], v[1], v[2], v[3]);
}

void glVertexAttribI1uiv(GLuint index, const GLuint* v)
{
    glVertexAttribI1ui(index, v[0]);
}

void glVertexAttribI2uiv(GLuint index, const GLuint* v)
{
    glVertexAttribI2ui(index, v[0], v[1]);
}

void glVertexAttribI3uiv(GLuint index, const GLuint* v)
{
    glVertexAttribI3ui(index, v[0], v[1], v[2]);
}

void glVertexAttribI4uiv(GLuint index, const GLuint* v)
{
    glVertexAttribI4ui(index, v[0], v[1], v[2], v[3]);
}

void glUniform1uiv(GLint location, GLsizei count, const GLuint* value)
{
    for (int i = 0; i < count; i++)
        glUniform1ui(location, value[i * 1 + 0]);
}

void glUniform2uiv(GLint location, GLsizei count, const GLuint* value)
{
    for (int i = 0; i < count; i++)
        glUniform2ui(location, value[i * 2 + 0], value[i * 2 + 1]);
}

void glUniform3uiv(GLint location, GLsizei count, const GLuint* value)
{
    for (int i = 0; i < count; i++)
        glUniform3ui(location, value[i * 3 + 0], value[i * 3 + 1], value[i * 3 + 2]);
}

void glUniform4uiv(GLint location, GLsizei count, const GLuint* value)
{
    for (int i = 0; i < count; i++)
        glUniform4ui(location, value[i * 4 + 0], value[i * 4 + 1], value[i * 4 + 2], value[i * 4 + 3]);
}

void glVertexAttribP1uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value)
{
    glVertexAttribP1ui(index, type, normalized, value[0]);
}

void glTexCoordP1uiv(GLenum type, const GLuint* coords)
{
    glTexCoordP1ui(type, coords[0]);
}

void glMultiTexCoordP1uiv(GLenum texture, GLenum type, const GLuint* coords)
{
    glMultiTexCoordP1ui(texture, type, coords[0]);
}

void glUniform1dv(GLint location, GLsizei count, const GLdouble* value)
{
    for (int i = 0; i < count; i++)
        glUniform1d(location, value[i * 1 + 0]);
}

void glUniform2dv(GLint location, GLsizei count, const GLdouble* value)
{
    for (int i = 0; i < count; i++)
        glUniform2d(location, value[i * 2 + 0], value[i * 2 + 1]);
}

void glUniform3dv(GLint location, GLsizei count, const GLdouble* value)
{
    for (int i = 0; i < count; i++)
        glUniform3d(location, value[i * 3 + 0], value[i * 3 + 1], value[i * 3 + 2]);
}

void glUniform4dv(GLint location, GLsizei count, const GLdouble* value)
{
    for (int i = 0; i < count; i++)
        glUniform4d(location, value[i * 4 + 0], value[i * 4 + 1], value[i * 4 + 2], value[i * 4 + 3]);
}

void glProgramUniform1iv(GLuint program, GLint location, GLsizei count, const GLint* value)
{
    for (int i = 0; i < count; i++)
        glProgramUniform1i(program, location, value[i * 1 + 0]);
}

void glProgramUniform1fv(GLuint program, GLint location, GLsizei count, const GLfloat* value)
{
    for (int i = 0; i < count; i++)
        glProgramUniform1f(program, location, value[i * 1 + 0]);
}

void glProgramUniform1dv(GLuint program, GLint location, GLsizei count, const GLdouble* value)
{
    for (int i = 0; i < count; i++)
        glProgramUniform1d(program, location, value[i * 1 + 0]);
}

void glProgramUniform1uiv(GLuint program, GLint location, GLsizei count, const GLuint* value)
{
    for (int i = 0; i < count; i++)
        glProgramUniform1ui(program, location, value[i * 1 + 0]);
}

void glProgramUniform2iv(GLuint program, GLint location, GLsizei count, const GLint* value)
{
    for (int i = 0; i < count; i++)
        glProgramUniform2i(program, location, value[i * 2 + 0], value[i * 2 + 1]);
}

void glProgramUniform2fv(GLuint program, GLint location, GLsizei count, const GLfloat* value)
{
    for (int i = 0; i < count; i++)
        glProgramUniform2f(program, location, value[i * 2 + 0], value[i * 2 + 1]);
}

void glProgramUniform2dv(GLuint program, GLint location, GLsizei count, const GLdouble* value)
{
    for (int i = 0; i < count; i++)
        glProgramUniform2d(program, location, value[i * 2 + 0], value[i * 2 + 1]);
}

void glProgramUniform2uiv(GLuint program, GLint location, GLsizei count, const GLuint* value)
{
    for (int i = 0; i < count; i++)
        glProgramUniform2ui(program, location, value[i * 2 + 0], value[i * 2 + 1]);
}

void glProgramUniform3iv(GLuint program, GLint location, GLsizei count, const GLint* value)
{
    for (int i = 0; i < count; i++)
        glProgramUniform3i(program, location, value[i * 3 + 0], value[i * 3 + 1], value[i * 3 + 2]);
}

void glProgramUniform3fv(GLuint program, GLint location, GLsizei count, const GLfloat* value)
{
    for (int i = 0; i < count; i++)
        glProgramUniform3f(program, location, value[i * 3 + 0], value[i * 3 + 1], value[i * 3 + 2]);
}

void glProgramUniform3dv(GLuint program, GLint location, GLsizei count, const GLdouble* value)
{
    for (int i = 0; i < count; i++)
        glProgramUniform3d(program, location, value[i * 3 + 0], value[i * 3 + 1], value[i * 3 + 2]);
}

void glProgramUniform3uiv(GLuint program, GLint location, GLsizei count, const GLuint* value)
{
    for (int i = 0; i < count; i++)
        glProgramUniform3ui(program, location, value[i * 3 + 0], value[i * 3 + 1], value[i * 3 + 2]);
}

void glProgramUniform4iv(GLuint program, GLint location, GLsizei count, const GLint* value)
{
    for (int i = 0; i < count; i++)
        glProgramUniform4i(program, location, value[i * 4 + 0], value[i * 4 + 1], value[i * 4 + 2], value[i * 4 + 3]);
}

void glProgramUniform4fv(GLuint program, GLint location, GLsizei count, const GLfloat* value)
{
    for (int i = 0; i < count; i++)
        glProgramUniform4f(program, location, value[i * 4 + 0], value[i * 4 + 1], value[i * 4 + 2], value[i * 4 + 3]);
}

void glProgramUniform4dv(GLuint program, GLint location, GLsizei count, const GLdouble* value)
{
    for (int i = 0; i < count; i++)
        glProgramUniform4d(program, location, value[i * 4 + 0], value[i * 4 + 1], value[i * 4 + 2], value[i * 4 + 3]);
}

void glProgramUniform4uiv(GLuint program, GLint location, GLsizei count, const GLuint* value)
{
    for (int i = 0; i < count; i++)
        glProgramUniform4ui(program, location, value[i * 4 + 0], value[i * 4 + 1], value[i * 4 + 2], value[i * 4 + 3]);
}

void glVertexAttribL1dv(GLuint index, const GLdouble* v)
{
    glVertexAttribL1d(index, v[0]);
}

void glVertexAttribL2dv(GLuint index, const GLdouble* v)
{
    glVertexAttribL2d(index, v[0], v[1]);
}

void glVertexAttribL3dv(GLuint index, const GLdouble* v)
{
    glVertexAttribL3d(index, v[0], v[1], v[2]);
}

void glVertexAttribL4dv(GLuint index, const GLdouble* v)
{
    glVertexAttribL4d(index, v[0], v[1], v[2], v[3]);
}

void glIndexdv(const GLdouble* c)
{
    glIndexd(c[0]);
}

void glIndexfv(const GLfloat* c)
{
    glIndexf(c[0]);
}

void glIndexiv(const GLint* c)
{
    glIndexi(c[0]);
}

void glIndexsv(const GLshort* c)
{
    glIndexs(c[0]);
}

void glIndexubv(const GLubyte* c)
{
    glIndexub(c[0]);
}

void glRectdv(const GLdouble* v1, const GLdouble* v2)
{
    glRectd(v1[0], v1[1], v2[0], v2[1]);
}

void glRectfv(const GLfloat* v1, const GLfloat* v2)
{
    glRectf(v1[0], v1[1], v2[0], v2[1]);
}

void glRectiv(const GLint* v1, const GLint* v2)
{
    glRecti(v1[0], v1[1], v2[0], v2[1]);
}

void glRectsv(const GLshort* v1, const GLshort* v2)
{
    glRects(v1[0], v1[1], v2[0], v2[1]);
}

void glEdgeFlagv(const GLboolean* flag)
{
    glEdgeFlag(flag[0]);
}

/*
 * to-do:
 * perhaps get rid of `params_fixed`
 */

void glFogiv(GLenum pname, const GLint* params)
{
    if (pname != GL_FOG_COLOR) {
        glFogi(pname, params[0]);
    }

    const GLfloat params_fixed[4] = {
        params[0],
        params[1],
        params[2],
        params[3]
    };

    glFogfv(pname, params_fixed);
}

void glLightiv(GLenum light, GLenum pname, const GLint* params)
{
    const GLfloat params_fixed[4] = {
        params[0],
        params[1],
        params[2],
        params[3]
    };

    glLightfv(light, pname, params_fixed);
}

void glLightModeliv(GLenum pname, const GLint* params)
{
    const GLfloat params_fixed[4] = {
        params[0],
        params[1],
        params[2],
        params[3]
    };

    glLightModelfv(pname, params_fixed);
}

void glMaterialiv(GLenum face, GLenum pname, const GLint* params)
{
    const GLfloat params_fixed[4] = {
        params[0],
        params[1],
        params[2],
        params[3]
    };

    glMaterialfv(face, pname, params_fixed);
}

void glPolygonStipple(const GLubyte* mask)
{
    int *pmask = (int*)mask;

    pb_push(SGL_CMD_POLYGONSTIPPLE);
    for (int i = 0; i < 32 * 32 / 4; i++)
        pb_push(pmask[i]);
}

void glTexEnvfv(GLenum target, GLenum pname, const GLfloat* params)
{
    if (pname == GL_TEXTURE_ENV_COLOR) {
        pb_push(SGL_CMD_TEXENVFV);
        pb_push(target);
        pb_push(pname);
        pb_pushf(params[0]);
        pb_pushf(params[1]);
        pb_pushf(params[2]);
        pb_pushf(params[3]);
        return;
    }

    glTexEnvf(target, pname, params[0]);
}

void glTexEnviv(GLenum target, GLenum pname, const GLint* params)
{
    if (pname == GL_TEXTURE_ENV_COLOR) {
        pb_push(SGL_CMD_TEXENVFV);
        pb_push(target);
        pb_push(pname);
        pb_push(params[0]);
        pb_push(params[1]);
        pb_push(params[2]);
        pb_push(params[3]);
        return;
    }

    glTexEnvi(target, pname, params[0]);
}

/*
 * to-do: perhaps fix the three functions below, they may
 * not function properly (see what i did there)
 */

void glTexGendv(GLenum coord, GLenum pname, const GLdouble* params)
{
    glTexGend(coord, pname, params[0]);
}

void glTexGenfv(GLenum coord, GLenum pname, const GLfloat* params)
{
    glTexGenf(coord, pname, params[0]);
}

void glTexGeniv(GLenum coord, GLenum pname, const GLint* params)
{
    glTexGeni(coord, pname, params[0]);
}

void glFeedbackBuffer(GLsizei size, GLenum type, GLfloat* buffer)
{
    pb_push(SGL_CMD_FEEDBACKBUFFER);
    pb_push(size);
    pb_push(type);

    glimpl_submit();
    memcpy(buffer, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), size * sizeof(float));
}

void glSelectBuffer(GLsizei size, GLuint* buffer)
{
    pb_push(SGL_CMD_SELECTBUFFER);
    pb_push(size);

    glimpl_submit();
    memcpy(buffer, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), size * sizeof(float));
}

void glMap1d(GLenum target, GLdouble u1, GLdouble u2, GLint stride, GLint order, const GLdouble* points)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(order);
    for (int i = 0; i < order; i++)
        pb_pushf(points[i * stride]);
    
    pb_push(SGL_CMD_MAP1F); /* not a mistake; cant handle doubles */
    pb_push(target);
    pb_pushf(u1);
    pb_pushf(u2);
    pb_push(stride);
    pb_push(order);
}

void glMap1f(GLenum target, GLfloat u1, GLfloat u2, GLint stride, GLint order, const GLfloat* points)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(order);
    for (int i = 0; i < order; i++)
        pb_pushf(points[i * stride]);
    
    pb_push(SGL_CMD_MAP1F);
    pb_push(target);
    pb_pushf(u1);
    pb_pushf(u2);
    pb_push(stride);
    pb_push(order);
}

/*
 * to-do: 2 functions below are probably wrong
 */

void glMap2d(GLenum target, GLdouble u1, GLdouble u2, GLint ustride, GLint uorder, GLdouble v1, GLdouble v2, GLint vstride, GLint vorder, const GLdouble* points)
{ 
    STUB();
    glMap1d(target, u1, u2, ustride, uorder, points);
    glMap1d(target, v1, v2, vstride, vorder, points);
}

void glMap2f(GLenum target, GLfloat u1, GLfloat u2, GLint ustride, GLint uorder, GLfloat v1, GLfloat v2, GLint vstride, GLint vorder, const GLfloat* points)
{ 
    STUB();
    glMap1f(target, u1, u2, ustride, uorder, points);
    glMap1f(target, v1, v2, vstride, vorder, points);
}

void glPixelMapfv(GLenum map, GLsizei mapsize, const GLfloat* values)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(mapsize);
    for (int i = 0; i < mapsize; i++)
        pb_pushf(values[i]);

    pb_push(SGL_CMD_PIXELMAPFV);
    pb_push(map);
    pb_push(mapsize);
}

void glPixelMapuiv(GLenum map, GLsizei mapsize, const GLuint* values)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(mapsize);
    for (int i = 0; i < mapsize; i++)
        pb_push(values[i]);

    pb_push(SGL_CMD_PIXELMAPUIV);
    pb_push(map);
    pb_push(mapsize);
}

void glPixelMapusv(GLenum map, GLsizei mapsize, const GLushort* values)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(mapsize);
    for (int i = 0; i < mapsize; i++)
        pb_push(values[i]);

    pb_push(SGL_CMD_PIXELMAPUIV); /* not a mistake; don't passthru short */
    pb_push(map);
    pb_push(mapsize);
}

void glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels)
{
    STUB();
}

void glGetClipPlane(GLenum plane, GLdouble* equation)
{
    pb_push(SGL_CMD_GETCLIPPLANE);
    pb_push(plane);
    pb_push(0); /* is dst float? */

    glimpl_submit();
    memcpy(equation, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), 4 * sizeof(double));
}

void glGetClipPlanef(GLenum plane, GLfloat* equation)
{
    pb_push(SGL_CMD_GETCLIPPLANE);
    pb_push(plane);
    pb_push(1); /* is dst float? */

    glimpl_submit();
    memcpy(equation, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), 4 * sizeof(float));
}

void glGetLightfv(GLenum light, GLenum pname, GLfloat* params)
{
    int size = 1;
    switch (pname) {
    case GL_AMBIENT:
    case GL_DIFFUSE:
    case GL_SPECULAR:
    case GL_POSITION:
        size = 4;
        break;
    case GL_SPOT_DIRECTION:
        size = 3;
        break;
    }

    pb_push(SGL_CMD_GETLIGHTFV);
    pb_push(light);
    pb_push(pname);

    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), size * sizeof(float));
}

void glGetLightiv(GLenum light, GLenum pname, GLint* params)
{
    int size = 1;
    switch (pname) {
    case GL_AMBIENT:
    case GL_DIFFUSE:
    case GL_SPECULAR:
    case GL_POSITION:
        size = 4;
        break;
    case GL_SPOT_DIRECTION:
        size = 3;
        break;
    }

    pb_push(SGL_CMD_GETLIGHTIV);
    pb_push(light);
    pb_push(pname);

    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), size * sizeof(int));
}

void glGetMapdv(GLenum target, GLenum query, GLdouble* v)
{
    STUB();
}

void glGetMapfv(GLenum target, GLenum query, GLfloat* v)
{
    STUB();
}

void glGetMapiv(GLenum target, GLenum query, GLint* v)
{
    STUB();
}

void glGetMaterialfv(GLenum face, GLenum pname, GLfloat* params)
{
    int size = 1;
    switch (pname) {
    case GL_AMBIENT:
    case GL_DIFFUSE:
    case GL_SPECULAR:
    case GL_EMISSION:
        size = 4;
        break;
    case GL_COLOR_INDEXES:
        size = 3;
        break;
    }

    pb_push(SGL_CMD_GETMATERIALFV);
    pb_push(face);
    pb_push(pname);

    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), size * sizeof(float));
}

void glGetMaterialiv(GLenum face, GLenum pname, GLint* params)
{
    int size = 1;
    switch (pname) {
    case GL_AMBIENT:
    case GL_DIFFUSE:
    case GL_SPECULAR:
    case GL_EMISSION:
        size = 4;
        break;
    case GL_COLOR_INDEXES:
        size = 3;
        break;
    }

    pb_push(SGL_CMD_GETMATERIALIV);
    pb_push(face);
    pb_push(pname);

    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), size * sizeof(int));
}

void glGetPixelMapfv(GLenum map, GLfloat* values)
{
    STUB();
}

void glGetPixelMapuiv(GLenum map, GLuint* values)
{
    STUB();
}

void glGetPixelMapusv(GLenum map, GLushort* values)
{
    STUB();
}

void glGetPolygonStipple(GLubyte* mask)
{
    STUB();
}

void glGetTexEnvfv(GLenum target, GLenum pname, GLfloat* params)
{
    pb_push(SGL_CMD_GETTEXENVFV);
    pb_push(target);
    pb_push(pname);
    
    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(float) + ((pname == GL_TEXTURE_ENV_COLOR) * (3 * sizeof(float))));
}

void glGetTexEnviv(GLenum target, GLenum pname, GLint* params)
{
    pb_push(SGL_CMD_GETTEXENVIV);
    pb_push(target);
    pb_push(pname);
    
    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(int) + ((pname == GL_TEXTURE_ENV_COLOR) * (3 * sizeof(int))));
}

void glGetTexGendv(GLenum coord, GLenum pname, GLdouble* params)
{
    pb_push(SGL_CMD_GETTEXGENDV);
    pb_push(coord);
    pb_push(pname);
    
    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(double) + ((pname == GL_OBJECT_PLANE || pname == GL_EYE_PLANE) * (3 * sizeof(double))));
}

void glGetTexGenfv(GLenum coord, GLenum pname, GLfloat* params)
{
    pb_push(SGL_CMD_GETTEXGENFV);
    pb_push(coord);
    pb_push(pname);
    
    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(float) + ((pname == GL_OBJECT_PLANE || pname == GL_EYE_PLANE) * (3 * sizeof(float))));
}

void glGetTexGeniv(GLenum coord, GLenum pname, GLint* params)
{
    pb_push(SGL_CMD_GETTEXGENIV);
    pb_push(coord);
    pb_push(pname);
    
    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(int) + ((pname == GL_OBJECT_PLANE || pname == GL_EYE_PLANE) * (3 * sizeof(int))));
}

void glCallLists(GLsizei n, GLenum type, const void* lists)
{
    for (int i = 0; i < n; i++) {
        switch (type) {
        case GL_BYTE:
        case GL_UNSIGNED_BYTE:
            glCallList(((unsigned char*)lists)[i]);
            break;
        case GL_SHORT:
        case GL_UNSIGNED_SHORT:
            glCallList(((unsigned short*)lists)[i]);
            break;
        case GL_INT:
        case GL_UNSIGNED_INT:
            glCallList(((unsigned int*)lists)[i]);
            break;
        case GL_FLOAT:
            glCallList(((float*)lists)[i]);
            break;
        /*
         * GL_2_BYTES GL_3_BYTES GL_4_BYTES
         */
        }
    }
}

void glTexParameterfv(GLenum target, GLenum pname, const GLfloat* params)
{
    if (pname != GL_TEXTURE_BORDER_COLOR && pname != GL_TEXTURE_SWIZZLE_RGBA) {
        glTexParameterf(target, pname, params[0]);
        return;
    }

    glimpl_texture_parameter_v(SGL_CMD_TEXPARAMETERFV, target, pname, params);
}

void glTexParameteriv(GLenum target, GLenum pname, const GLint* params)
{
    if (pname != GL_TEXTURE_BORDER_COLOR && pname != GL_TEXTURE_SWIZZLE_RGBA) {
        glTexParameteri(target, pname, params[0]);
        return;
    }

    glimpl_texture_parameter_v(SGL_CMD_TEXPARAMETERIV, target, pname, params);
}

void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat* params)
{
    glimpl_get_texture_parameter(SGL_CMD_GETTEXPARAMETERFV, target, pname, params);
}

void glGetTexParameteriv(GLenum target, GLenum pname, GLint* params)
{
    glimpl_get_texture_parameter(SGL_CMD_GETTEXPARAMETERIV, target, pname, params);
}

void glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat* params)
{
    glimpl_get_texture_level_parameter(SGL_CMD_GETTEXLEVELPARAMETERFV, target, level, pname, params);
}

void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* params)
{
    glimpl_get_texture_level_parameter(SGL_CMD_GETTEXLEVELPARAMETERIV, target, level, pname, params);
}

void glGetPointerv(GLenum pname, GLvoid **params)
{
    switch (pname) {
    case GL_VERTEX_ARRAY_POINTER:
        *params = (void*)glimpl_vertex_ptr.pointer;
        break;
    case GL_NORMAL_ARRAY_POINTER:
        *params = (void*)glimpl_normal_ptr.pointer;
        break;
    case GL_COLOR_ARRAY_POINTER:
        *params = (void*)glimpl_color_ptr.pointer;
        break;
    case GL_TEXTURE_COORD_ARRAY_POINTER:
        *params = (void*)glimpl_tex_coord_ptr[glimpl_client_active_texture].pointer;
        break;
    default:
        *params = (void*)0;
        break;
    }
}

GLboolean glAreTexturesResident(GLsizei n, const GLuint* textures, GLboolean* residences)
{
    GLboolean res = GL_TRUE;
    GLboolean *p = residences;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_ARETEXTURESRESIDENT);
        pb_push(textures[i]);

        glimpl_submit();
        *p = pb_read(SGL_OFFSET_REGISTER_RETVAL);
        
        res = res && *p++;
    }

    return res;
}

void glPrioritizeTextures(GLsizei n, const GLuint* textures, const GLfloat* priorities)
{
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_PRIORITIZETEXTURES);
        pb_push(textures[i]);
        pb_pushf(priorities[i]);
    }
}

void glCompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void* data)
{
    glimpl_submit();

    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(imageSize / 4);
    pb_memcpy((void*)data, imageSize);

    pb_push(SGL_CMD_COMPRESSEDTEXIMAGE3D);
    pb_push(target);
    pb_push(level);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
    pb_push(depth);
    pb_push(border);
    pb_push(imageSize);

    glimpl_submit();
}

void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void* data)
{
    glimpl_submit();

    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(imageSize / 4);
    pb_memcpy((void*)data, imageSize);

    pb_push(SGL_CMD_COMPRESSEDTEXIMAGE2D);
    pb_push(target);
    pb_push(level);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
    pb_push(border);
    pb_push(imageSize);

    glimpl_submit();
}

void glCompressedTexImage1D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLint border, GLsizei imageSize, const void* data)
{
    glimpl_submit();

    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(imageSize / 4);
    pb_memcpy((void*)data, imageSize);

    pb_push(SGL_CMD_COMPRESSEDTEXIMAGE1D);
    pb_push(target);
    pb_push(level);
    pb_push(internalformat);
    pb_push(width);
    pb_push(border);
    pb_push(imageSize);

    glimpl_submit();
}

void glCompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void* data)
{
    glimpl_compressed_texture_subimage(SGL_CMD_COMPRESSEDTEXSUBIMAGE3D, 3, 
            target, level, xoffset, yoffset, zoffset, width, height, depth, format, imageSize, data);
}

void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void* data)
{
    glimpl_compressed_texture_subimage(SGL_CMD_COMPRESSEDTEXSUBIMAGE2D, 2, 
            target, level, xoffset, yoffset, 0, width, height, 0, format, imageSize, data);
}

void glCompressedTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLsizei imageSize, const void* data)
{
    glimpl_compressed_texture_subimage(SGL_CMD_COMPRESSEDTEXSUBIMAGE1D, 1, 
            target, level, xoffset, 0, 0, width, 0, 0, format, imageSize, data);
}

void glGetCompressedTexImage(GLenum target, GLint level, void* img)
{
    /*
     * to-do: do
     */
    STUB();
}

void glLoadTransposeMatrixf(const GLfloat* m)
{
    pb_push(SGL_CMD_LOADTRANSPOSEMATRIXF);
    for (int i = 0; i < 16; i++)
        pb_pushf(m[i]);
}

void glLoadTransposeMatrixd(const GLdouble* m)
{
    pb_push(SGL_CMD_LOADTRANSPOSEMATRIXF);
    for (int i = 0; i < 16; i++)
        pb_pushf(m[i]);
}

void glMultTransposeMatrixf(const GLfloat* m)
{
    pb_push(SGL_CMD_MULTTRANSPOSEMATRIXF);
    for (int i = 0; i < 16; i++)
        pb_pushf(m[i]);
}

void glMultTransposeMatrixd(const GLdouble* m)
{
    pb_push(SGL_CMD_MULTTRANSPOSEMATRIXF);
    for (int i = 0; i < 16; i++)
        pb_pushf(m[i]);
}

void glMultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount)
{
    for (int i = 0; i < drawcount; i++)
        glDrawArrays(mode, first[i], count[i]);
}

void glMultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const void* const*indices, GLsizei drawcount)
{
    for (int i = 0; i < drawcount; i++)
        glDrawElements(mode, count[i], type, indices[i]);
}

void glPointParameterfv(GLenum pname, const GLfloat* params)
{
    glPointParameterf(pname, params[0]);
}

void glPointParameteriv(GLenum pname, const GLint* params)
{
    glPointParameteri(pname, params[0]);
}

void glFogCoordfv(const GLfloat* coord)
{
    glFogCoordf(coord[0]);
}

void glFogCoorddv(const GLdouble* coord)
{
    glFogCoordd(coord[0]);
}

void glDeleteQueries(GLsizei n, const GLuint* ids)
{
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_DELETEQUERIES);
        pb_push(ids[i]);
    }
}

void glGetQueryiv(GLenum target, GLenum pname, GLint* params)
{
    pb_push(SGL_CMD_GETQUERYIV);
    pb_push(target);
    pb_push(pname);

    glimpl_submit();
    *params = pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glGetQueryObjectiv(GLuint id, GLenum pname, GLint* params)
{
    pb_push(SGL_CMD_GETQUERYOBJECTIV);
    pb_push(id);
    pb_push(pname);

    glimpl_submit();
    *params = pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params)
{
    pb_push(SGL_CMD_GETQUERYOBJECTUIV);
    pb_push(id);
    pb_push(pname);

    glimpl_submit();
    *params = pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data)
{
    glimpl_buffer_subdata(SGL_CMD_BUFFERSUBDATA, target, offset, size, data);
}

void glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* data)
{
    STUB();
}

void* glMapBuffer(GLenum target, GLenum access)
{
    return glimpl_map_buffer_range(SGL_CMD_MAPBUFFER, target, 0, 0, access);
}

void glGetBufferParameteriv(GLenum target, GLenum pname, GLint* params)
{
    glimpl_get_buffer_parameter(SGL_CMD_GETBUFFERPARAMETERIV, target, pname, params);
}

void glGetBufferPointerv(GLenum target, GLenum pname, void* *params)
{
    STUB();
}

void glDrawBuffers(GLsizei n, const GLenum* bufs)
{
    pb_push(SGL_CMD_DRAWBUFFERS);
    pb_push(n);
    for (int i = 0; i < n; i++)
        pb_push(bufs[i]);
}

void glBindAttribLocation(GLuint program, GLuint index, const GLchar* name)
{
    pb_push(SGL_CMD_BINDATTRIBLOCATION);
    pb_push(program);
    pb_push(index);
    push_string(name);
}

void glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLint* size, GLenum* type, GLchar* name)
{
    pb_push(SGL_CMD_GETACTIVEATTRIB);
    pb_push(program);
    pb_push(index);
    pb_push(512); /* bufSize */
    
    glimpl_submit();
    GLsizei len;
    memcpy(&len, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(GLsizei));
    memcpy(size, (void*)((size_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V) + sizeof(GLsizei)), sizeof(GLint));
    memcpy(type, (void*)((size_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V) + sizeof(GLsizei) + sizeof(GLint)), sizeof(GLenum));
    memcpy(name, (void*)((size_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V) + sizeof(GLsizei) + sizeof(GLint) + sizeof(GLenum)), len);

    if (length)
        *length = len;
}

void glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLint* size, GLenum* type, GLchar* name)
{
    pb_push(SGL_CMD_GETACTIVEUNIFORM);
    pb_push(program);
    pb_push(index);
    pb_push(512); /* bufSize */
    
    glimpl_submit();
    GLsizei len;
    memcpy(&len, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(GLsizei));
    memcpy(size, (void*)((size_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V) + sizeof(GLsizei)), sizeof(GLint));
    memcpy(type, (void*)((size_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V) + sizeof(GLsizei) + sizeof(GLint)), sizeof(GLenum));
    memcpy(name, (void*)((size_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V) + sizeof(GLsizei) + sizeof(GLint) + sizeof(GLenum)), len);

    if (length)
        *length = len;
}

void glGetAttachedShaders(GLuint program, GLsizei maxCount, GLsizei* count, GLuint* shaders)
{
    pb_push(SGL_CMD_GETATTACHEDSHADERS);
    pb_push(program);
    pb_push(128); /* maxCount */

    glimpl_submit();
    GLsizei len;
    memcpy(&len, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(GLsizei));
    memcpy(shaders, (void*)((size_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V) + sizeof(GLsizei)), len * sizeof(GLuint));

    if (count)
        *count = len;
}

void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog)
{
    pb_push(SGL_CMD_GETPROGRAMINFOLOG);
    pb_push(program);
    pb_push(512); /* bufSize */

    glimpl_submit();
    GLsizei len;
    memcpy(&len, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(GLsizei));
    memcpy(infoLog, (void*)((size_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V) + sizeof(GLsizei)), len);
    
    if (length)
        *length = len;
}

void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog)
{
    pb_push(SGL_CMD_GETSHADERINFOLOG);
    pb_push(shader);
    pb_push(512); /* bufSize */

    glimpl_submit();
    GLsizei len;
    memcpy(&len, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(GLsizei));
    memcpy(infoLog, (void*)((size_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V) + sizeof(GLsizei)), len);
    
    if (length)
        *length = len;
}

void glGetShaderSource(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* source)
{
    pb_push(SGL_CMD_GETSHADERSOURCE);
    pb_push(shader);
    pb_push(3072); /* bufSize */

    glimpl_submit();
    GLsizei len;
    memcpy(&len, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(GLsizei));
    memcpy(source, (void*)((size_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V) + sizeof(GLsizei)), len);

    if (length)
        *length = len;
}

void glGetUniformfv(GLuint program, GLint location, GLfloat* params)
{
    pb_push(SGL_CMD_GETUNIFORMFV);
    pb_push(program);
    pb_push(location);

    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL), sizeof(float));
}

void glGetUniformiv(GLuint program, GLint location, GLint* params)
{
    pb_push(SGL_CMD_GETUNIFORMIV);
    pb_push(program);
    pb_push(location);

    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL), sizeof(int));
}

void glGetVertexAttribdv(GLuint index, GLenum pname, GLdouble* params)
{
    pb_push(SGL_CMD_GETVERTEXATTRIBDV);
    pb_push(index);
    pb_push(pname);

    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(GLdouble) + ((pname == GL_CURRENT_VERTEX_ATTRIB) * 3 * sizeof(GLdouble)));
}

void glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat* params)
{
    pb_push(SGL_CMD_GETVERTEXATTRIBFV);
    pb_push(index);
    pb_push(pname);

    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(float) + ((pname == GL_CURRENT_VERTEX_ATTRIB) * 3 * sizeof(float)));
}

void glGetVertexAttribiv(GLuint index, GLenum pname, GLint* params)
{
    pb_push(SGL_CMD_GETVERTEXATTRIBIV);
    pb_push(index);
    pb_push(pname);

    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(int) + ((pname == GL_CURRENT_VERTEX_ATTRIB) * 3 * sizeof(int)));
}

void glGetVertexAttribPointerv(GLuint index, GLenum pname, void* *pointer)
{
    *pointer = glimpl_vaps[index].ptr;
}

void glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 2 * 2);
    for (int i = 0; i < count * 2 * 2; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_UNIFORMMATRIX2FV);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 3 * 3);
    for (int i = 0; i < count * 3 * 3; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_UNIFORMMATRIX3FV);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glVertexAttrib4Nbv(GLuint index, const GLbyte* v)
{
    pb_push(SGL_CMD_VERTEXATTRIB4NBV);
    pb_push(index);
    pb_push(v[0]);
    pb_push(v[1]);
    pb_push(v[2]);
    pb_push(v[3]);
}

void glVertexAttrib4Niv(GLuint index, const GLint* v)
{
    pb_push(SGL_CMD_VERTEXATTRIB4NIV);
    pb_push(index);
    pb_push(v[0]);
    pb_push(v[1]);
    pb_push(v[2]);
    pb_push(v[3]);
}

void glVertexAttrib4Nsv(GLuint index, const GLshort* v)
{
    pb_push(SGL_CMD_VERTEXATTRIB4NSV);
    pb_push(index);
    pb_push(v[0]);
    pb_push(v[1]);
    pb_push(v[2]);
    pb_push(v[3]);
}

void glVertexAttrib4Nuiv(GLuint index, const GLuint* v)
{
    pb_push(SGL_CMD_VERTEXATTRIB4NUIV);
    pb_push(index);
    pb_push(v[0]);
    pb_push(v[1]);
    pb_push(v[2]);
    pb_push(v[3]);
}

void glVertexAttrib4Nusv(GLuint index, const GLushort* v)
{
    pb_push(SGL_CMD_VERTEXATTRIB4NUSV);
    pb_push(index);
    pb_push(v[0]);
    pb_push(v[1]);
    pb_push(v[2]);
    pb_push(v[3]);
}

void glVertexAttrib4bv(GLuint index, const GLbyte* v)
{
    pb_push(SGL_CMD_VERTEXATTRIB4BV);
    pb_push(index);
    pb_push(v[0]);
    pb_push(v[1]);
    pb_push(v[2]);
    pb_push(v[3]);
}

void glVertexAttrib4iv(GLuint index, const GLint* v)
{
    pb_push(SGL_CMD_VERTEXATTRIB4IV);
    pb_push(index);
    pb_push(v[0]);
    pb_push(v[1]);
    pb_push(v[2]);
    pb_push(v[3]);
}

void glVertexAttrib4ubv(GLuint index, const GLubyte* v)
{
    pb_push(SGL_CMD_VERTEXATTRIB4UBV);
    pb_push(index);
    pb_push(v[0]);
    pb_push(v[1]);
    pb_push(v[2]);
    pb_push(v[3]);
}

void glVertexAttrib4uiv(GLuint index, const GLuint* v)
{
    pb_push(SGL_CMD_VERTEXATTRIB4UIV);
    pb_push(index);
    pb_push(v[0]);
    pb_push(v[1]);
    pb_push(v[2]);
    pb_push(v[3]);
}

void glVertexAttrib4usv(GLuint index, const GLushort* v)
{
    pb_push(SGL_CMD_VERTEXATTRIB4USV);
    pb_push(index);
    pb_push(v[0]);
    pb_push(v[1]);
    pb_push(v[2]);
    pb_push(v[3]);
}

void glUniformMatrix2x3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 2 * 3);
    for (int i = 0; i < count * 2 * 3; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_UNIFORMMATRIX2X3FV);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glUniformMatrix3x2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 3 * 2);
    for (int i = 0; i < count * 3 * 2; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_UNIFORMMATRIX3X2FV);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glUniformMatrix2x4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 2 * 4);
    for (int i = 0; i < count * 2 * 4; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_UNIFORMMATRIX2X4FV);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glUniformMatrix4x2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 2 * 4);
    for (int i = 0; i < count * 2 * 4; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_UNIFORMMATRIX4X2FV);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glUniformMatrix3x4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 3 * 4);
    for (int i = 0; i < count * 3 * 4; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_UNIFORMMATRIX3X4FV);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glUniformMatrix4x3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 3 * 4);
    for (int i = 0; i < count * 3 * 4; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_UNIFORMMATRIX4X3FV);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value)
{
    pb_push(SGL_CMD_CLEARBUFFERIV);
    pb_push(buffer);
    pb_push(drawbuffer);
    
    pb_push(value[0]);
    pb_push(buffer == GL_COLOR ? value[1] : 0);
    pb_push(buffer == GL_COLOR ? value[2] : 0);
    pb_push(buffer == GL_COLOR ? value[3] : 0);
}

void glClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value)
{
    glimpl_clear_buffer(SGL_CMD_CLEARBUFFERUIV, false, GL_UNSIGNED_INT, 0, buffer, drawbuffer, (void*)value);
}

void glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value)
{
    glimpl_clear_buffer(SGL_CMD_CLEARBUFFERFV, false, GL_FLOAT, 0, buffer, drawbuffer, (void*)value);
}

void glGetBooleani_v(GLenum target, GLuint index, GLboolean* data)
{
    pb_push(SGL_CMD_GETBOOLEANI_V);
    pb_push(target);
    pb_push(index);
    glimpl_submit();
    GL_GET_MEMCPY_RETVAL_EX(target, data, GLboolean);
}

void glGetIntegeri_v(GLenum target, GLuint index, GLint* data)
{
    pb_push(SGL_CMD_GETINTEGERI_V);
    pb_push(target);
    pb_push(index);
    glimpl_submit();
    GL_GET_MEMCPY_RETVAL_EX(target, data, GLint);
}

void glTransformFeedbackVaryings(GLuint program, GLsizei count, const GLchar* const*varyings, GLenum bufferMode)
{
    glimpl_submit();

    for (int i = 0; i < count; i++) {
        pb_push(SGL_CMD_TRANSFORMFEEDBACKVARYINGS);
        pb_push(program);
        // pb_push(1);
        push_string(varyings[i]);
        pb_push(bufferMode);
    }

    glimpl_submit();
}

void glGetTransformFeedbackVarying(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLsizei* size, GLenum* type, GLchar* name)
{
    pb_push(SGL_CMD_GETTRANSFORMFEEDBACKVARYING);
    pb_push(program);
    pb_push(index);
    pb_push(bufSize);
    glimpl_submit();

    // hope that data fits in SGL_OFFSET_REGISTER_RETVAL_V
    // could lead to corrupting the push buffer, unless garbage
    // is overwritten with enough data before submit
    uintptr_t ptr = (uintptr_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V);
    GLsizei length_notptr;

    memcpy(&length_notptr, (void*)(ptr + 0), sizeof(*length));
    if (length != NULL)
        *length = length_notptr;

    memcpy(size, (void*)(ptr + sizeof(*length)), sizeof(*size));
    memcpy(type, (void*)(ptr + sizeof(*length) + sizeof(*size)), sizeof(*type));
    memcpy(name, (void*)(ptr + sizeof(*length) + sizeof(*size) + sizeof(*type)), length_notptr);
}

void glGetVertexAttribIiv(GLuint index, GLenum pname, GLint* params)
{
    // lazy, probably wrong
    // applies to functions below aswell
    glGetVertexAttribiv(index, pname, params);
}

void glGetVertexAttribIuiv(GLuint index, GLenum pname, GLuint* params)
{
    glGetVertexAttribiv(index, pname, (GLint*)params);
}

void glVertexAttribI4bv(GLuint index, const GLbyte* v)
{
    glVertexAttrib4bv(index, v);
}

void glVertexAttribI4sv(GLuint index, const GLshort* v)
{
    glVertexAttrib4sv(index, v);
}

void glVertexAttribI4ubv(GLuint index, const GLubyte* v)
{
    glVertexAttrib4ubv(index, v);
}

void glVertexAttribI4usv(GLuint index, const GLushort* v)
{
    glVertexAttrib4usv(index, v);
}

void glGetUniformuiv(GLuint program, GLint location, GLuint* params)
{
    glGetUniformiv(program, location, (GLint*)params);
}

GLint glGetFragDataLocation(GLuint program, const GLchar* name)
{
    pb_push(SGL_CMD_GETFRAGDATALOCATION);
    pb_push(program);
    push_string(name);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glTexParameterIiv(GLenum target, GLenum pname, const GLint* params)
{
    glTexParameteriv(target, pname, params);
}

void glTexParameterIuiv(GLenum target, GLenum pname, const GLuint* params)
{
    glTexParameteriv(target, pname, (const GLint*)params);
}

void glGetTexParameterIiv(GLenum target, GLenum pname, GLint* params)
{
    glGetTexParameteriv(target, pname, params);
}

void glGetTexParameterIuiv(GLenum target, GLenum pname, GLuint* params)
{
    glGetTexParameteriv(target, pname, (GLint*)params);
}

void glDeleteRenderbuffers(GLsizei n, const GLuint* renderbuffers)
{
    // to-do: should glDelete* end w/ submit?
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_DELETERENDERBUFFERS);
        // pb_push(1);
        pb_push(renderbuffers[i]);
    }
}

void glGenRenderbuffers(GLsizei n, GLuint* renderbuffers)
{
    GLuint *p = renderbuffers;
    
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_GENRENDERBUFFERS);
        // pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint* params)
{
    glimpl_get_renderbuffer_parameter(SGL_CMD_GETRENDERBUFFERPARAMETERIV, target, pname, params);
}

void glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers)
{
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_DELETEFRAMEBUFFERS);
        // pb_push(1);
        pb_push(framebuffers[i]);
    }
}

void glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint* params)
{
    glimpl_get_framebuffer_parameter(SGL_CMD_GETFRAMEBUFFERATTACHMENTPARAMETERIV, true, target, attachment, pname, params);
}

void* glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)
{
    return glimpl_map_buffer_range(SGL_CMD_MAPBUFFERRANGE, target, offset, length, access);
}

void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount)
{
    if (!is_value_likely_an_offset(indices)) {
        fprintf(stderr, "glDrawElementsInstanced: indices isn't an offset, fallback to glDrawElements\n");
        glDrawElements(mode, count, type, indices);
        return;
    }

    pb_push(SGL_CMD_DRAWELEMENTSINSTANCED);
    pb_push(mode);
    pb_push(count);
    pb_push(type);
    pb_push((uint32_t)(uintptr_t)indices);
    pb_push(instancecount);
}

void glGetUniformIndices(GLuint program, GLsizei uniformCount, const GLchar* const*uniformNames, GLuint* uniformIndices)
{
    GLuint *p = uniformIndices;
    
    for (int i = 0; i < uniformCount; i++) {
        pb_push(SGL_CMD_GETUNIFORMINDICES);
        pb_push(program);
        // pb_push(1);
        push_string(uniformNames[i]);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGetActiveUniformsiv(GLuint program, GLsizei uniformCount, const GLuint* uniformIndices, GLenum pname, GLint* params)
{
    GLint *p = params;
    
    for (int i = 0; i < uniformCount; i++) {
        pb_push(SGL_CMD_GETACTIVEUNIFORMSIV);
        pb_push(program);
        // pb_push(1);
        pb_push(uniformIndices[i]);
        pb_push(pname);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGetActiveUniformName(GLuint program, GLuint uniformIndex, GLsizei bufSize, GLsizei* length, GLchar* uniformName)
{
    pb_push(SGL_CMD_GETACTIVEUNIFORMNAME);
    pb_push(program);
    pb_push(uniformIndex);
    pb_push(bufSize);

    glimpl_submit();

    uintptr_t ptr = (uintptr_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V);
    GLsizei length_notptr;

    memcpy(&length_notptr, (void*)(ptr + 0), sizeof(*length));
    if (length != NULL)
        *length = length_notptr;

    memcpy(uniformName, (void*)(ptr + sizeof(*length)), length_notptr);
}

GLuint glGetUniformBlockIndex(GLuint program, const GLchar* uniformBlockName)
{
    pb_push(SGL_CMD_GETUNIFORMBLOCKINDEX);
    pb_push(program);
    push_string(uniformBlockName);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glGetActiveUniformBlockiv(GLuint program, GLuint uniformBlockIndex, GLenum pname, GLint* params)
{
    pb_push(SGL_CMD_GETACTIVEUNIFORMBLOCKIV);
    pb_push(program);
    pb_push(uniformBlockIndex);
    pb_push(pname);

    glimpl_submit();
    *params = pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glGetActiveUniformBlockName(GLuint program, GLuint uniformBlockIndex, GLsizei bufSize, GLsizei* length, GLchar* uniformBlockName)
{
    pb_push(SGL_CMD_GETACTIVEUNIFORMBLOCKNAME);
    pb_push(program);
    pb_push(uniformBlockIndex);
    pb_push(bufSize);

    glimpl_submit();

    uintptr_t ptr = (uintptr_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V);
    GLsizei length_notptr;

    memcpy(&length_notptr, (void*)(ptr + 0), sizeof(*length));
    if (length != NULL)
        *length = length_notptr;

    memcpy(uniformBlockName, (void*)(ptr + sizeof(*length)), length_notptr);
}

void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex)
{
    if (!is_value_likely_an_offset(indices)) {
        fprintf(stderr, "glDrawElementsBaseVertex: indices isn't an offset, fallback to glDrawElements\n");
        glDrawElements(mode, count, type, indices);
        return;
    }

    pb_push(SGL_CMD_DRAWELEMENTSBASEVERTEX);
    pb_push(mode);
    pb_push(count);
    pb_push(type);
    pb_push((uint32_t)(uintptr_t)indices);
    pb_push(basevertex);
}

void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices, GLint basevertex)
{
    if (!is_value_likely_an_offset(indices)) {
        fprintf(stderr, "glDrawRangeElementsBaseVertex: indices isn't an offset, fallback to glDrawElements\n");
        glDrawElements(mode, count, type, indices);
        return;
    }

    glimpl_push_vertex_attrib_pointers(count);
    glimpl_push_client_pointers(mode, count);

    pb_push(SGL_CMD_DRAWRANGEELEMENTSBASEVERTEX);
    pb_push(mode);
    pb_push(start);
    pb_push(end);
    pb_push(count);
    pb_push(type);
    pb_push((uint32_t)(uintptr_t)indices);
    pb_push(basevertex);
}

void glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount, GLint basevertex)
{
    if (!is_value_likely_an_offset(indices)) {
        fprintf(stderr, "glDrawElementsInstancedBaseVertex: indices isn't an offset, fallback to glDrawElements\n");
        glDrawElements(mode, count, type, indices);
        return;
    }

    pb_push(SGL_CMD_DRAWELEMENTSINSTANCEDBASEVERTEX);
    pb_push(mode);
    pb_push(count);
    pb_push(type);
    pb_push((uint32_t)(uintptr_t)indices);
    pb_push(instancecount);
    pb_push(basevertex);
}

void glMultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, const void* const*indices, GLsizei drawcount, const GLint* basevertex)
{
    for (int i = 0; i < drawcount; i++)
        glDrawElementsBaseVertex(mode, count[i], type, indices[i], basevertex[i]);
}

GLsync glFenceSync(GLenum condition, GLbitfield flags)
{
    // pb_push(SGL_CMD_FENCESYNC);
    // pb_push(condition);
    // pb_push(flags);
    
    // glimpl_submit();

    // return (GLsync)pb_read64(SGL_OFFSET_REGISTER_RETVAL_V);
    return NULL;
}

GLboolean glIsSync(GLsync sync)
{
    // pb_push(SGL_CMD_ISSYNC);
    // pb_push(((uint64_t*)&sync)[0]);
    // pb_push(((uint64_t*)&sync)[1]);

    // glimpl_submit();

    // return pb_read(SGL_OFFSET_REGISTER_RETVAL_V);
    return GL_TRUE;
}

void glDeleteSync(GLsync sync)
{
    // pb_push(SGL_CMD_DELETESYNC);
    // pb_push(((uint64_t*)&sync)[0]);
    // pb_push(((uint64_t*)&sync)[1]);

    // glimpl_submit();
}

GLenum glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
    // pb_push(SGL_CMD_CLIENTWAITSYNC);
    // pb_push(((uint64_t*)&sync)[0]);
    // pb_push(((uint64_t*)&sync)[1]);
    // pb_push(flags);
    // pb_push((&timeout)[0]);
    // pb_push((&timeout)[1]);

    // glimpl_submit();

    // return pb_read(SGL_OFFSET_REGISTER_RETVAL_V);

    // SGL_OFFSET_REGISTER_RETVAL_V causes issues on network
    // to-do: fix?
    return GL_CONDITION_SATISFIED;
}

void glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
    // pb_push(SGL_CMD_WAITSYNC);
    // pb_push(((uint64_t*)&sync)[0]);
    // pb_push(((uint64_t*)&sync)[1]);
    // pb_push(flags);
    // pb_push((&timeout)[0]);
    // pb_push((&timeout)[1]);

    // glimpl_submit();
}

void glGetInteger64v(GLenum pname, GLint64* data)
{
    // wrong
    glGetIntegerv(pname, (GLint*)data);
}

void glGetSynciv(GLsync sync, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* values)
{
    // fprintf(stderr, "glGetSynciv: sync is stub\n");
}

void glGetInteger64i_v(GLenum target, GLuint index, GLint64* data)
{
    // wrong
    glGetIntegeri_v(target, index, (GLint*)data);
}

void glGetBufferParameteri64v(GLenum target, GLenum pname, GLint64* params)
{
    // wrong
    glGetBufferParameteriv(target, pname, (GLint*)params);
}

void glGetMultisamplefv(GLenum pname, GLuint index, GLfloat* val)
{
    pb_push(SGL_CMD_GETMULTISAMPLEFV);
    pb_push(pname);
    pb_push(index);

    glimpl_submit();

    memcpy(val, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(float) * 2);
}

void glBindFragDataLocationIndexed(GLuint program, GLuint colorNumber, GLuint index, const GLchar* name)
{
    pb_push(SGL_CMD_BINDFRAGDATALOCATIONINDEXED);
    pb_push(program);
    pb_push(colorNumber);
    pb_push(index);
    push_string(name);
}

GLint glGetFragDataIndex(GLuint program, const GLchar* name)
{
    pb_push(SGL_CMD_GETFRAGDATAINDEX);
    pb_push(program);
    push_string(name);

    glimpl_submit();

    return pb_read(SGL_OFFSET_REGISTER_RETVAL); 
}

void glGenSamplers(GLsizei count, GLuint* samplers)
{
    GLuint *p = samplers;

    for (int i = 0; i < count; i++) {
        pb_push(SGL_CMD_GENSAMPLERS);
        // pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glDeleteSamplers(GLsizei count, const GLuint* samplers)
{
    for (int i = 0; i < count; i++) {
        pb_push(SGL_CMD_DELETESAMPLERS);
        // pb_push(1);

        pb_push(samplers[i]);
    }
}

void glSamplerParameteriv(GLuint sampler, GLenum pname, const GLint* param)
{
    STUB();
}

void glSamplerParameterfv(GLuint sampler, GLenum pname, const GLfloat* param)
{
    STUB();
}

void glSamplerParameterIiv(GLuint sampler, GLenum pname, const GLint* param)
{
    STUB();
}

void glSamplerParameterIuiv(GLuint sampler, GLenum pname, const GLuint* param)
{
    STUB();
}

void glGetSamplerParameteriv(GLuint sampler, GLenum pname, GLint* params)
{
    STUB();
}

void glGetSamplerParameterIiv(GLuint sampler, GLenum pname, GLint* params)
{
    STUB();
}

void glGetSamplerParameterfv(GLuint sampler, GLenum pname, GLfloat* params)
{
    STUB();
}

void glGetSamplerParameterIuiv(GLuint sampler, GLenum pname, GLuint* params)
{
    STUB();
}

void glGetQueryObjecti64v(GLuint id, GLenum pname, GLint64* params)
{
    STUB();
}

void glVertexAttribP2uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value)
{
    STUB();
}

void glVertexAttribP3uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value)
{
    STUB();
}

void glVertexAttribP4uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value)
{
    STUB();
}

void glVertexP2uiv(GLenum type, const GLuint* value)
{
    STUB();
}

void glVertexP3uiv(GLenum type, const GLuint* value)
{
    STUB();
}

void glVertexP4uiv(GLenum type, const GLuint* value)
{
    STUB();
}

void glTexCoordP2uiv(GLenum type, const GLuint* coords)
{
    STUB();
}

void glTexCoordP3uiv(GLenum type, const GLuint* coords)
{
    STUB();
}

void glTexCoordP4uiv(GLenum type, const GLuint* coords)
{
    STUB();
}

void glMultiTexCoordP2uiv(GLenum texture, GLenum type, const GLuint* coords)
{
    STUB();
}

void glMultiTexCoordP3uiv(GLenum texture, GLenum type, const GLuint* coords)
{
    STUB();
}

void glMultiTexCoordP4uiv(GLenum texture, GLenum type, const GLuint* coords)
{
    STUB();
}

void glNormalP3uiv(GLenum type, const GLuint* coords)
{
    STUB();
}

void glColorP3uiv(GLenum type, const GLuint* color)
{
    STUB();
}

void glColorP4uiv(GLenum type, const GLuint* color)
{
    STUB();
}

void glSecondaryColorP3uiv(GLenum type, const GLuint* color)
{
    STUB();
}

void glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer)
{
    bool client_managed = !is_value_likely_an_offset(pointer);

    glimpl_vaps[index] = (struct gl_vertex_attrib_pointer){ 
        .index = index,
        .size = size,
        .type = type,
        .normalized = GL_FALSE,
        .stride = stride,
        .ptr = (void*)pointer,
        .enabled = false,
        .client_managed = client_managed
    };

    // if (!client_managed) {
        pb_push(SGL_CMD_VERTEXATTRIBIPOINTER);
        pb_push(index);
        pb_push(size);
        pb_push(type);
        pb_push(stride);
        pb_push((int)((uintptr_t)pointer));
    // }
}

void glDrawArraysIndirect(GLenum mode, const void* indirect)
{
    /*
     * to-do: instead of using this function, check if GL_DRAW_INDIRECT_BUFFER is bound
     */
    if (!is_value_likely_an_offset(indirect)) {
        fprintf(stderr, "glDrawArraysIndirect: expected GL_DRAW_INDIRECT_BUFFER\n");
        return;
    }
    
    pb_push(SGL_CMD_DRAWARRAYSINDIRECT);
    pb_push(mode);
    pb_push((int)(uintptr_t)indirect);
}

void glDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect)
{
    /*
     * to-do: instead of using this function, check if GL_DRAW_INDIRECT_BUFFER is bound
     */
    if (!is_value_likely_an_offset(indirect)) {
        fprintf(stderr, "glDrawElementsIndirect: expected GL_DRAW_INDIRECT_BUFFER\n");
        return;
    }
    
    pb_push(SGL_CMD_DRAWELEMENTSINDIRECT);
    pb_push(mode);
    pb_push(type);
    pb_push((int)(uintptr_t)indirect);
}

void glUniformMatrix2dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    /*
     * to-do: support doubles
     * im too lazy to actually support doubles so we cast to floats
     */
    int elem_count = count * 2 * 2;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glUniformMatrix2fv(location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glUniformMatrix3dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 3 * 3;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glUniformMatrix3fv(location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glUniformMatrix4dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 4 * 4;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glUniformMatrix4fv(location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glUniformMatrix2x3dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 2 * 3;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glUniformMatrix2x3fv(location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glUniformMatrix2x4dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 2 * 4;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glUniformMatrix2x4fv(location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glUniformMatrix3x2dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 3 * 2;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glUniformMatrix3x2fv(location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glUniformMatrix3x4dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 3 * 4;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glUniformMatrix3x4fv(location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glUniformMatrix4x2dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 4 * 2;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glUniformMatrix4x2fv(location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glUniformMatrix4x3dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 4 * 3;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glUniformMatrix4x3fv(location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glGetUniformdv(GLuint program, GLint location, GLdouble* params)
{
    pb_push(SGL_CMD_GETUNIFORMDV);
    pb_push(program);
    pb_push(location);

    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(double));
}

GLint glGetSubroutineUniformLocation(GLuint program, GLenum shadertype, const GLchar* name)
{
    pb_push(SGL_CMD_GETSUBROUTINEUNIFORMLOCATION);
    pb_push(program);
    pb_push(shadertype);
    push_string(name);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

GLuint glGetSubroutineIndex(GLuint program, GLenum shadertype, const GLchar* name)
{
    pb_push(SGL_CMD_GETSUBROUTINEINDEX);
    pb_push(program);
    pb_push(shadertype);
    push_string(name);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glGetActiveSubroutineUniformiv(GLuint program, GLenum shadertype, GLuint index, GLenum pname, GLint* values)
{
    pb_push(SGL_CMD_GETACTIVESUBROUTINEUNIFORMIV);
    pb_push(program);
    pb_push(shadertype);
    pb_push(index);
    pb_push(pname);

    glimpl_submit();
    switch (pname) {
    case GL_COMPATIBLE_SUBROUTINES: {
        int num = pb_read(SGL_OFFSET_REGISTER_RETVAL);
        int *vals = values;
        for (int i = 0; i < pb_read(SGL_OFFSET_REGISTER_RETVAL); i++)
            *vals++ = pb_read(SGL_OFFSET_REGISTER_RETVAL_V + (i * 4));
        break;
    }
    default:
        *values = pb_read(SGL_OFFSET_REGISTER_RETVAL);
        break;
    }
}

void glGetActiveSubroutineUniformName(GLuint program, GLenum shadertype, GLuint index, GLsizei bufsize, GLsizei* length, GLchar* name)
{
    pb_push(SGL_CMD_GETACTIVESUBROUTINEUNIFORMNAME);
    pb_push(program);
    pb_push(shadertype);
    pb_push(index);
    pb_push(bufsize);

    glimpl_submit();

    uintptr_t ptr = (uintptr_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V);
    GLsizei length_notptr;

    memcpy(&length_notptr, (void*)(ptr + 0), sizeof(*length));
    if (length != NULL)
        *length = length_notptr;

    memcpy(name, (void*)(ptr + sizeof(*length)), length_notptr);
}

void glGetActiveSubroutineName(GLuint program, GLenum shadertype, GLuint index, GLsizei bufsize, GLsizei* length, GLchar* name)
{
    pb_push(SGL_CMD_GETACTIVESUBROUTINENAME);
    pb_push(program);
    pb_push(shadertype);
    pb_push(index);
    pb_push(bufsize);

    glimpl_submit();

    uintptr_t ptr = (uintptr_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V);
    GLsizei length_notptr;

    memcpy(&length_notptr, (void*)(ptr + 0), sizeof(*length));
    if (length != NULL)
        *length = length_notptr;

    memcpy(name, (void*)(ptr + sizeof(*length)), length_notptr);
}

void glUniformSubroutinesuiv(GLenum shadertype, GLsizei count, const GLuint* indices)
{
    pb_push(SGL_CMD_UNIFORMSUBROUTINESUIV);
    pb_push(shadertype);
    pb_push(count);

    for (int i = 0; i < count; i++)
        pb_push(indices[i]);
}

void glGetUniformSubroutineuiv(GLenum shadertype, GLint location, GLuint* params)
{
    pb_push(SGL_CMD_GETUNIFORMSUBROUTINEUIV);
    pb_push(shadertype);
    pb_push(location);

    glimpl_submit();
    *params = pb_read(SGL_OFFSET_REGISTER_RETVAL_V);
}

void glGetProgramStageiv(GLuint program, GLenum shadertype, GLenum pname, GLint* values)
{
    pb_push(SGL_CMD_GETPROGRAMSTAGEIV);
    pb_push(program);
    pb_push(shadertype);
    pb_push(pname);

    glimpl_submit();
    *values = pb_read(SGL_OFFSET_REGISTER_RETVAL_V);
}

void glPatchParameterfv(GLenum pname, const GLfloat* values)
{
    int count = pname == GL_PATCH_DEFAULT_OUTER_LEVEL ? 4 : 2;

    pb_push(SGL_CMD_PATCHPARAMETERFV);
    pb_push(pname);
    pb_push(count);

    for (int i = 0; i < count; i++)
        pb_pushf(values[i]);
}

void glDeleteTransformFeedbacks(GLsizei n, const GLuint* ids)
{
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_DELETETRANSFORMFEEDBACKS);
        // pb_push(1);

        pb_push(ids[i]);
    }
}

void glGenTransformFeedbacks(GLsizei n, GLuint* ids)
{
    GLuint *p = ids;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_GENTRANSFORMFEEDBACKS);
        // pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGetQueryIndexediv(GLenum target, GLuint index, GLenum pname, GLint* params)
{
    pb_push(SGL_CMD_GETQUERYINDEXEDIV);
    pb_push(target);
    pb_push(index);
    pb_push(pname);

    glimpl_submit();
    *params = pb_read(SGL_OFFSET_REGISTER_RETVAL_V);
}

void glShaderBinary(GLsizei count, const GLuint* shaders, GLenum binaryformat, const void* binary, GLsizei length)
{
    pb_push(SGL_CMD_SHADERBINARY);
    pb_push(count);
    pb_push(binaryformat);
    pb_push(length);
    for (int i = 0; i < count; i++)
        pb_push(shaders[i]);
    pb_memcpy((void*)binary, length);
}

void glGetShaderPrecisionFormat(GLenum shadertype, GLenum precisiontype, GLint* range, GLint* precision)
{
    pb_push(SGL_CMD_GETSHADERPRECISIONFORMAT);
    pb_push(shadertype);
    pb_push(precisiontype);
    pb_push(range[0]);
    pb_push(range[1]);

    glimpl_submit();
    *precision = pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glGetProgramBinary(GLuint program, GLsizei bufSize, GLsizei* length, GLenum* binaryFormat, void* binary)
{
    // pb_push(SGL_CMD_GETPROGRAMBINARY);
    // pb_push(program);
    // pb_push(bufSize);

    STUB();
}

void glProgramBinary(GLuint program, GLenum binaryFormat, const void* binary, GLsizei length)
{
    pb_push(SGL_CMD_SHADERBINARY);
    pb_push(program);
    pb_push(binaryFormat);
    pb_push(length);
    pb_memcpy((void*)binary, length);
}

GLuint glCreateShaderProgramv(GLenum type, GLsizei count, const GLchar* const*strings)
{
    pb_push(SGL_CMD_CREATESHADERPROGRAMV);
    pb_push(type);
    pb_push(count);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}


void glDeleteProgramPipelines(GLsizei n, const GLuint* pipelines)
{
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_DELETEPROGRAMPIPELINES);
        // pb_push(1);

        pb_push(pipelines[i]);
    }
}

void glGenProgramPipelines(GLsizei n, GLuint* pipelines)
{
    GLuint *p = pipelines;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_GENPROGRAMPIPELINES);
        // pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGetProgramPipelineiv(GLuint pipeline, GLenum pname, GLint* params)
{
    pb_push(SGL_CMD_GETPROGRAMPIPELINEIV);
    pb_push(pipeline);
    pb_push(pname);

    glimpl_submit();
    *params = pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glProgramUniformMatrix2fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 2 * 2);
    for (int i = 0; i < count * 2 * 2; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_PROGRAMUNIFORMMATRIX2FV);
    pb_push(program);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glProgramUniformMatrix3fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 3 * 3);
    for (int i = 0; i < count * 3 * 3; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_PROGRAMUNIFORMMATRIX3FV);
    pb_push(program);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glProgramUniformMatrix4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 4 * 4);
    for (int i = 0; i < count * 4 * 4; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_PROGRAMUNIFORMMATRIX4FV);
    pb_push(program);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glProgramUniformMatrix2dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 2 * 2;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glProgramUniformMatrix2fv(program, location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glProgramUniformMatrix3dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 3 * 3;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glProgramUniformMatrix3fv(program, location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glProgramUniformMatrix4dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 4 * 4;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glProgramUniformMatrix4fv(program, location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glProgramUniformMatrix2x3fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 2 * 3);
    for (int i = 0; i < count * 2 * 3; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_PROGRAMUNIFORMMATRIX2X3FV);
    pb_push(program);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glProgramUniformMatrix3x2fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 3 * 2);
    for (int i = 0; i < count * 3 * 2; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_PROGRAMUNIFORMMATRIX3X2FV);
    pb_push(program);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glProgramUniformMatrix2x4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 2 * 4);
    for (int i = 0; i < count * 2 * 4; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_PROGRAMUNIFORMMATRIX2X4FV);
    pb_push(program);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glProgramUniformMatrix4x2fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 4 * 2);
    for (int i = 0; i < count * 4 * 2; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_PROGRAMUNIFORMMATRIX4X2FV);
    pb_push(program);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glProgramUniformMatrix3x4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 3 * 4);
    for (int i = 0; i < count * 3 * 4; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_PROGRAMUNIFORMMATRIX3X4FV);
    pb_push(program);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glProgramUniformMatrix4x3fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 4 * 3);
    for (int i = 0; i < count * 4 * 3; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_PROGRAMUNIFORMMATRIX4X3FV);
    pb_push(program);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);
}

void glProgramUniformMatrix2x3dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 2 * 3;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glProgramUniformMatrix2x3fv(program, location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glProgramUniformMatrix3x2dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 3 * 2;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glProgramUniformMatrix3x2fv(program, location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glProgramUniformMatrix2x4dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 2 * 4;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glProgramUniformMatrix2x4fv(program, location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glProgramUniformMatrix4x2dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 4 * 2;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glProgramUniformMatrix4x2fv(program, location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glProgramUniformMatrix3x4dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 3 * 4;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glProgramUniformMatrix3x4fv(program, location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glProgramUniformMatrix4x3dv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLdouble* value)
{
    int elem_count = count * 4 * 3;
    for (int i = 0; i < elem_count; i++)
        glimpl_global_matrix_double_to_float[i] = value[i];
    glProgramUniformMatrix4x3fv(program, location, count, transpose, glimpl_global_matrix_double_to_float);
}

void glGetProgramPipelineInfoLog(GLuint pipeline, GLsizei bufSize, GLsizei* length, GLchar* infoLog)
{
    pb_push(SGL_CMD_GETPROGRAMPIPELINEINFOLOG);
    pb_push(pipeline);
    pb_push(bufSize);
    
    glimpl_submit();

    uintptr_t ptr = (uintptr_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V);
    GLsizei length_notptr;

    memcpy(&length_notptr, (void*)(ptr + 0), sizeof(*length));
    if (length != NULL)
        *length = length_notptr;

    memcpy(infoLog, (void*)(ptr + sizeof(*length)), length_notptr);
}

void glVertexAttribLPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer)
{
    bool client_managed = !is_value_likely_an_offset(pointer);

    glimpl_vaps[index] = (struct gl_vertex_attrib_pointer){ 
        .index = index,
        .size = size,
        .type = type,
        .normalized = GL_FALSE,
        .stride = stride,
        .ptr = (void*)pointer,
        .enabled = false,
        .client_managed = client_managed
    };

    // if (!client_managed) {
        pb_push(SGL_CMD_VERTEXATTRIBLPOINTER);
        pb_push(index);
        pb_push(size);
        pb_push(type);
        pb_push(stride);
        pb_push((int)((uintptr_t)pointer));
    // }
}

void glGetVertexAttribLdv(GLuint index, GLenum pname, GLdouble* params)
{
    // wrong
    glGetVertexAttribdv(index, pname, params);
}

void glViewportArrayv(GLuint first, GLsizei count, const GLfloat* v)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 4);
    for (int i = 0; i < count * 4; i++)
        pb_pushf(v[i]);

    pb_push(SGL_CMD_VIEWPORTARRAYV);
    pb_push(first);
    pb_push(count);
}

void glViewportIndexedfv(GLuint index, const GLfloat* v)
{
    glViewportIndexedf(index, v[0], v[1], v[2], v[3]);
}

void glScissorArrayv(GLuint first, GLsizei count, const GLint* v)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 4);
    for (int i = 0; i < count * 4; i++)
        pb_pushf(v[i]);

    pb_push(SGL_CMD_SCISSORARRAYV);
    pb_push(first);
    pb_push(count);
}

void glScissorIndexedv(GLuint index, const GLint* v)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(4);
    for (int i = 0; i < 4; i++)
        pb_pushf(v[i]);

    pb_push(SGL_CMD_SCISSORINDEXEDV);
    pb_push(index);
}

void glDepthRangeArrayv(GLuint first, GLsizei count, const GLdouble* v)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 2);
    for (int i = 0; i < count * 2; i++)
        pb_pushf(v[i]);

    pb_push(SGL_CMD_DEPTHRANGEARRAYV);
    pb_push(first);
    pb_push(count);
}

void glGetFloati_v(GLenum target, GLuint index, GLfloat* data)
{
    pb_push(SGL_CMD_GETINTEGERI_V);
    pb_push(target);
    pb_push(index);
    glimpl_submit();
    GL_GET_MEMCPY_RETVAL_EX(target, data, GLfloat);
}

void glGetDoublei_v(GLenum target, GLuint index, GLdouble* data)
{
    pb_push(SGL_CMD_GETDOUBLEI_V);
    pb_push(target);
    pb_push(index);
    glimpl_submit();
    GL_GET_MEMCPY_RETVAL_EX(target, data, GLfloat);
}

// 4.2

void glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount, GLuint baseinstance)
{
    /*
     * to-do: check if GL_ELEMENT_ARRAY_BUFFER is bound
     */
    if (!is_value_likely_an_offset(indices)) {
        fprintf(stderr, "glDrawElementsInstancedBaseInstance: expected GL_ELEMENT_ARRAY_BUFFER\n");
        return;
    }

    pb_push(SGL_CMD_DRAWELEMENTSINSTANCEDBASEINSTANCE);
    pb_push(mode);
    pb_push(count);
    pb_push(type);
    pb_push((uintptr_t)indices);
    pb_push(instancecount);
    pb_push(baseinstance);
}

void glDrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount, GLint basevertex, GLuint baseinstance)
{
    if (!is_value_likely_an_offset(indices)) {
        fprintf(stderr, "glDrawElementsInstancedBaseVertexBaseInstance: expected GL_ELEMENT_ARRAY_BUFFER\n");
        return;
    }

    pb_push(SGL_CMD_DRAWELEMENTSINSTANCEDBASEVERTEXBASEINSTANCE);
    pb_push(mode);
    pb_push(count);
    pb_push(type);
    pb_push((uintptr_t)indices);
    pb_push(instancecount);
    pb_push(basevertex);
    pb_push(baseinstance);
}

void glGetInternalformativ(GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, GLint* params)
{
    pb_push(SGL_CMD_GETINTERNALFORMATIV);
    pb_push(target);
    pb_push(internalformat);
    pb_push(pname);
    pb_push(bufSize);
    
    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), bufSize * sizeof(int));
}

void glGetActiveAtomicCounterBufferiv(GLuint program, GLuint bufferIndex, GLenum pname, GLint* params)
{
    pb_push(SGL_CMD_GETACTIVEATOMICCOUNTERBUFFERIV);
    pb_push(program);
    pb_push(bufferIndex);
    pb_push(pname);

    glimpl_submit();
    switch (pname) {
    case GL_ATOMIC_COUNTER_BUFFER_ACTIVE_ATOMIC_COUNTER_INDICES: {
        int num = pb_read(SGL_OFFSET_REGISTER_RETVAL);
        int *vals = params;
        for (int i = 0; i < pb_read(SGL_OFFSET_REGISTER_RETVAL); i++)
            *vals++ = pb_read(SGL_OFFSET_REGISTER_RETVAL_V + (i * 4));
        break;
    }
    default:
        *params = pb_read(SGL_OFFSET_REGISTER_RETVAL);
        break;
    }
}

// 4.3

void glClearBufferData(GLenum target, GLenum internalformat, GLenum format, GLenum type, const void* data)
{
    glimpl_buffer_clear_data(SGL_CMD_CLEARBUFFERDATA, false, target, internalformat, 0, 0, format, type, data);
}

void glClearBufferSubData(GLenum target, GLenum internalformat, GLintptr offset, GLsizeiptr size, GLenum format, GLenum type, const void* data)
{
    glimpl_buffer_clear_data(SGL_CMD_CLEARBUFFERSUBDATA, true, target, internalformat, offset, size, format, type, data);
}

void glGetFramebufferParameteriv(GLenum target, GLenum pname, GLint* params)
{
    glimpl_get_framebuffer_parameter(SGL_CMD_GETFRAMEBUFFERPARAMETERIV, false, target, 0, pname, params);
}

void glGetInternalformati64v(GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, GLint64* params)
{
    pb_push(SGL_CMD_GETINTERNALFORMATI64V);
    pb_push(target);
    pb_push(internalformat);
    pb_push(pname);
    pb_push(bufSize);
    
    glimpl_submit();
    memcpy(params, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), bufSize * sizeof(GLint64));
}

void glInvalidateFramebuffer(GLenum target, GLsizei numAttachments, const GLenum* attachments)
{
    glimpl_invalidate_framebuffer(SGL_CMD_INVALIDATEFRAMEBUFFER, false, target, numAttachments, attachments, 0, 0, 0, 0);
}

void glInvalidateSubFramebuffer(GLenum target, GLsizei numAttachments, const GLenum* attachments, GLint x, GLint y, GLsizei width, GLsizei height)
{
    glimpl_invalidate_framebuffer(SGL_CMD_INVALIDATESUBFRAMEBUFFER, true, target, numAttachments, attachments, x, y, width, height);
}

void glMultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride)
{
    for (int i = 0; i < drawcount; i++)
        glDrawArraysIndirect(mode, (char *)indirect + i * stride);
}

void glMultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride)
{
    for (int i = 0; i < drawcount; i++)
        glDrawElementsIndirect(mode, type, (char *)indirect + i * stride);
}

void glGetProgramInterfaceiv(GLuint program, GLenum programInterface, GLenum pname, GLint* params)
{
    pb_push(SGL_CMD_GETPROGRAMINTERFACEIV);
    pb_push(program);
    pb_push(programInterface);
    pb_push(pname);

    glimpl_submit();
    *params = pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

GLuint glGetProgramResourceIndex(GLuint program, GLenum programInterface, const GLchar* name)
{
    pb_push(SGL_CMD_GETPROGRAMRESOURCEINDEX);
    pb_push(program);
    pb_push(programInterface);
    push_string(name);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glGetProgramResourceName(GLuint program, GLenum programInterface, GLuint index, GLsizei bufSize, GLsizei* length, GLchar* name)
{
    pb_push(SGL_CMD_GETPROGRAMRESOURCENAME);
    pb_push(program);
    pb_push(programInterface);
    pb_push(index);
    pb_push(bufSize);
    
    glimpl_submit();

    uintptr_t ptr = (uintptr_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V);
    GLsizei length_notptr;

    memcpy(&length_notptr, (void*)(ptr + 0), sizeof(*length));
    if (length != NULL)
        *length = length_notptr;

    memcpy(name, (void*)(ptr + sizeof(*length)), length_notptr);
}

void glGetProgramResourceiv(GLuint program, GLenum programInterface, GLuint index, GLsizei propCount, const GLenum* props, GLsizei bufSize, GLsizei* length, GLint* params)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(propCount);
    for (int i = 0; i < propCount; i++)
        pb_push(props[i]);

    pb_push(SGL_CMD_GETPROGRAMRESOURCEIV);
    pb_push(program);
    pb_push(programInterface);
    pb_push(index);
    pb_push(propCount);
    pb_push(bufSize);

    glimpl_submit();

    uintptr_t ptr = (uintptr_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V);
    GLsizei length_notptr;

    memcpy(&length_notptr, (void*)(ptr + 0), sizeof(*length));
    if (length != NULL)
        *length = length_notptr;

    memcpy(params, (void*)(ptr + sizeof(*length)), length_notptr * 4);
}

GLint glGetProgramResourceLocation(GLuint program, GLenum programInterface, const GLchar* name)
{
    pb_push(SGL_CMD_GETPROGRAMRESOURCELOCATION);
    pb_push(program);
    pb_push(programInterface);
    push_string(name);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

GLint glGetProgramResourceLocationIndex(GLuint program, GLenum programInterface, const GLchar* name)
{
    pb_push(SGL_CMD_GETPROGRAMRESOURCELOCATIONINDEX);
    pb_push(program);
    pb_push(programInterface);
    push_string(name);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glDebugMessageControl(GLenum source, GLenum type, GLenum severity, GLsizei count, const GLuint* ids, GLboolean enabled)
{
    STUB();
}

void glDebugMessageInsert(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* buf)
{
    STUB();
}

void glDebugMessageCallback(GLDEBUGPROC callback, const void* userParam)
{
    STUB();
}

GLuint glGetDebugMessageLog(GLuint count, GLsizei bufSize, GLenum* sources, GLenum* types, GLuint* ids, GLenum* severities, GLsizei* lengths, GLchar* messageLog)
{
    return 0;
}

void glPushDebugGroup(GLenum source, GLuint id, GLsizei length, const GLchar* message)
{
    STUB();
}

void glObjectLabel(GLenum identifier, GLuint name, GLsizei length, const GLchar* label)
{
    STUB();
}

void glGetObjectLabel(GLenum identifier, GLuint name, GLsizei bufSize, GLsizei* length, GLchar* label)
{
    STUB();
}

void glObjectPtrLabel(const void* ptr, GLsizei length, const GLchar* label)
{
    STUB();
}

void glGetObjectPtrLabel(const void* ptr, GLsizei bufSize, GLsizei* length, GLchar* label)
{
    STUB();
}

// 4.4

void glBufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags)
{
    glimpl_buffer_store_data(SGL_CMD_BUFFERSTORAGE, target, size, data, flags);
}

void glClearTexImage(GLuint texture, GLint level, GLenum format, GLenum type, const void* data)
{
    pb_push(SGL_CMD_CLEARTEXIMAGE);
    pb_push(texture);
    pb_push(level);
    pb_push(format);
    pb_push(type);
    pb_push(*(unsigned int*)data);
}

void glClearTexSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* data)
{
    pb_push(SGL_CMD_CLEARTEXSUBIMAGE);
    pb_push(texture);
    pb_push(level);
    pb_push(xoffset);
    pb_push(yoffset);
    pb_push(zoffset);
    pb_push(width);
    pb_push(height);
    pb_push(depth);
    pb_push(format);
    pb_push(type);
    pb_push(*(unsigned int*)data);
}

void glBindBuffersRange(GLenum target, GLuint first, GLsizei count, const GLuint* buffers, const GLintptr* offsets, const GLsizeiptr* sizes)
{
    pb_push(SGL_CMD_BINDBUFFERSRANGE);
    pb_push(target);
    pb_push(first);
    pb_push(count);
    for (int i = 0; i < count; i++) {
        pb_push(buffers[i]);
        pb_push(offsets[i]);
        pb_push(sizes[i]);
    }
}

void glBindTextures(GLuint first, GLsizei count, const GLuint* textures)
{
    // to-do: have compatability option?
    // for (GLuint i = 0; i < count; i++) {
    //     glActiveTexture(GL_TEXTURE0 + first + i);
    //     glBindTexture(GL_TEXTURE_2D, textures[i]);
    // }

    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count);
    for (int i = 0; i < count; i++)
        pb_push(textures[i]);

    pb_push(SGL_CMD_BINDTEXTURES);
    pb_push(first);
    pb_push(count);
}

void glBindSamplers(GLuint first, GLsizei count, const GLuint* samplers)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count);
    for (int i = 0; i < count; i++)
        pb_push(samplers[i]);

    pb_push(SGL_CMD_BINDSAMPLERS);
    pb_push(first);
    pb_push(count);
}

void glBindImageTextures(GLuint first, GLsizei count, const GLuint* textures)
{
    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count);
    for (int i = 0; i < count; i++)
        pb_push(textures[i]);

    pb_push(SGL_CMD_BINDIMAGETEXTURES);
    pb_push(first);
    pb_push(count);
}

void glBindVertexBuffers(GLuint first, GLsizei count, const GLuint* buffers, const GLintptr* offsets, const GLsizei* strides)
{
    pb_push(SGL_CMD_BINDVERTEXBUFFERS);
    pb_push(first);
    pb_push(count);
    for (int i = 0; i < count; i++) {
        pb_push(buffers[i]);
        pb_push(offsets[i]);
        pb_push(strides[i]);
    }
}

// 4.5

void glCreateTransformFeedbacks(GLsizei n, GLuint* ids)
{
    GLuint *p = ids;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_CREATETRANSFORMFEEDBACKS);
        // pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGetTransformFeedbackiv(GLuint xfb, GLenum pname, GLint* param)
{
    pb_push(SGL_CMD_GETTRANSFORMFEEDBACKIV);
    pb_push(xfb);
    pb_push(pname);
    
    glimpl_submit();
    *param = pb_read(SGL_OFFSET_REGISTER_RETVAL_V);
}

void glGetTransformFeedbacki_v(GLuint xfb, GLenum pname, GLuint index, GLint* param)
{
    pb_push(SGL_CMD_GETTRANSFORMFEEDBACKI_V);
    pb_push(xfb);
    pb_push(pname);
    pb_push(index);
    
    glimpl_submit();
    *param = pb_read(SGL_OFFSET_REGISTER_RETVAL_V);
}

void glGetTransformFeedbacki64_v(GLuint xfb, GLenum pname, GLuint index, GLint64* param)
{
    pb_push(SGL_CMD_GETTRANSFORMFEEDBACKI64_V);
    pb_push(xfb);
    pb_push(pname);
    pb_push(index);
    
    glimpl_submit();
    *param = pb_read64(SGL_OFFSET_REGISTER_RETVAL_V);
}

void glCreateBuffers(GLsizei n, GLuint* buffers)
{
    GLuint *p = buffers;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_CREATEBUFFERS);
        // pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glNamedBufferStorage(GLuint buffer, GLsizeiptr size, const void* data, GLbitfield flags)
{
    glimpl_buffer_store_data(SGL_CMD_NAMEDBUFFERSTORAGE, buffer, size, data, flags);
}

void glNamedBufferData(GLuint buffer, GLsizeiptr size, const void* data, GLenum usage)
{
    glimpl_buffer_store_data(SGL_CMD_NAMEDBUFFERDATA, buffer, size, data, usage);
}

void glNamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data)
{
    glimpl_buffer_subdata(SGL_CMD_NAMEDBUFFERSUBDATA, buffer, offset, size, data);
}

void glClearNamedBufferData(GLuint buffer, GLenum internalformat, GLenum format, GLenum type, const void* data)
{
    glimpl_buffer_clear_data(SGL_CMD_CLEARNAMEDBUFFERDATA, false, buffer, internalformat, 0, 0, format, type, data);
}

void glClearNamedBufferSubData(GLuint buffer, GLenum internalformat, GLintptr offset, GLsizeiptr size, GLenum format, GLenum type, const void* data)
{
    glimpl_buffer_clear_data(SGL_CMD_CLEARNAMEDBUFFERSUBDATA, true, buffer, internalformat, offset, size, format, type, data);
}

void* glMapNamedBuffer(GLuint buffer, GLenum access)
{
    return glimpl_map_buffer_range(SGL_CMD_MAPNAMEDBUFFER, buffer, 0, 0, access);
}

void* glMapNamedBufferRange(GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access)
{
    return glimpl_map_buffer_range(SGL_CMD_MAPNAMEDBUFFERRANGE, buffer, offset, length, access);
}

void glGetNamedBufferParameteriv(GLuint buffer, GLenum pname, GLint* params)
{
    glimpl_get_buffer_parameter(SGL_CMD_GETNAMEDBUFFERPARAMETERIV, buffer, pname, params);
}

void glGetNamedBufferParameteri64v(GLuint buffer, GLenum pname, GLint64* params)
{
    glGetNamedBufferParameteriv(buffer, pname, (GLint*)params);
}

void glGetNamedBufferPointerv(GLuint buffer, GLenum pname, void* *params)
{
    /*
     * stub, refer to glGetBufferPointerv
     */
    STUB();
    return;
}

void glGetNamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, void* data)
{
    /*
     * stub, refer to glGetBufferSubData
     */
    STUB();
    return;
}

void glCreateFramebuffers(GLsizei n, GLuint* framebuffers)
{
    GLuint *p = framebuffers;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_CREATEFRAMEBUFFERS);
        // pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glNamedFramebufferDrawBuffers(GLuint framebuffer, GLsizei n, const GLenum* bufs)
{
    pb_push(SGL_CMD_NAMEDFRAMEBUFFERDRAWBUFFERS);
    pb_push(framebuffer);
    pb_push(n);
    for (int i = 0; i < n; i++)
        pb_push(bufs[i]);
}

void glInvalidateNamedFramebufferData(GLuint framebuffer, GLsizei numAttachments, const GLenum* attachments)
{
    glimpl_invalidate_framebuffer(SGL_CMD_INVALIDATENAMEDFRAMEBUFFERDATA, false, framebuffer, 
            numAttachments, attachments, 0, 0, 0, 0);
}

void glInvalidateNamedFramebufferSubData(GLuint framebuffer, GLsizei numAttachments, const GLenum* attachments, GLint x, GLint y, GLsizei width, GLsizei height)
{
    glimpl_invalidate_framebuffer(SGL_CMD_INVALIDATENAMEDFRAMEBUFFERSUBDATA, true, framebuffer, 
            numAttachments, attachments, x, y, width, height);
}

void glClearNamedFramebufferiv(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLint* value)
{
    glimpl_clear_buffer(SGL_CMD_CLEARNAMEDFRAMEBUFFERIV, true, GL_INT, framebuffer, buffer, drawbuffer, (void*)value);
}

void glClearNamedFramebufferuiv(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLuint* value)
{
    glimpl_clear_buffer(SGL_CMD_CLEARNAMEDFRAMEBUFFERUIV, true, GL_UNSIGNED_INT, framebuffer, buffer, drawbuffer, (void*)value);
}

void glClearNamedFramebufferfv(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLfloat* value)
{
    glimpl_clear_buffer(SGL_CMD_CLEARNAMEDFRAMEBUFFERFV, true, GL_FLOAT, framebuffer, buffer, drawbuffer, (void*)value);
}

void glGetNamedFramebufferParameteriv(GLuint framebuffer, GLenum pname, GLint* param)
{
    glimpl_get_framebuffer_parameter(SGL_CMD_GETNAMEDFRAMEBUFFERATTACHMENTPARAMETERIV, false, framebuffer, 0, pname, param);
}

void glGetNamedFramebufferAttachmentParameteriv(GLuint framebuffer, GLenum attachment, GLenum pname, GLint* params)
{
    glimpl_get_framebuffer_parameter(SGL_CMD_GETNAMEDFRAMEBUFFERATTACHMENTPARAMETERIV, true, framebuffer, attachment, pname, params);
}

void glCreateRenderbuffers(GLsizei n, GLuint* renderbuffers)
{
    GLuint *p = renderbuffers;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_CREATERENDERBUFFERS);
        // pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGetNamedRenderbufferParameteriv(GLuint renderbuffer, GLenum pname, GLint* params)
{
    glimpl_get_renderbuffer_parameter(SGL_CMD_GETNAMEDRENDERBUFFERPARAMETERIV, renderbuffer, pname, params);
}

void glCreateTextures(GLenum target, GLsizei n, GLuint* textures)
{
    GLuint *p = textures;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_CREATETEXTURES);
        pb_push(target);
        // pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glTextureSubImage1D(GLuint texture, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const void* pixels)
{
    glimpl_texture_subimage(SGL_CMD_TEXTURESUBIMAGE1D, 1,
            texture, level, xoffset, 0, 0, width, 0, 0, format, type, pixels);
}

void glTextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels)
{
    glimpl_texture_subimage(SGL_CMD_TEXTURESUBIMAGE2D, 2,
            texture, level, xoffset, yoffset, 0, width, height, 0, format, type, pixels);
}

void glTextureSubImage3D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels)
{
    glimpl_texture_subimage(SGL_CMD_TEXTURESUBIMAGE3D, 3,
            texture, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
}

void glCompressedTextureSubImage1D(GLuint texture, GLint level, GLint xoffset, GLsizei width, GLenum format, GLsizei imageSize, const void* data)
{
    glimpl_compressed_texture_subimage(SGL_CMD_COMPRESSEDTEXTURESUBIMAGE1D, 1, 
            texture, level, xoffset, 0, 0, width, 0, 0, format, imageSize, data);
}

void glCompressedTextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void* data)
{
    glimpl_compressed_texture_subimage(SGL_CMD_COMPRESSEDTEXTURESUBIMAGE2D, 2, 
            texture, level, xoffset, yoffset, 0, width, height, 0, format, imageSize, data);
}

void glCompressedTextureSubImage3D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void* data)
{
    glimpl_compressed_texture_subimage(SGL_CMD_COMPRESSEDTEXTURESUBIMAGE3D, 3, 
            texture, level, xoffset, yoffset, zoffset, width, height, depth, format, imageSize, data);
}

void glTextureParameterfv(GLuint texture, GLenum pname, const GLfloat* param)
{
    glimpl_texture_parameter_v(SGL_CMD_TEXTUREPARAMETERFV, texture, pname, param);
}

void glTextureParameterIiv(GLuint texture, GLenum pname, const GLint* params)
{
    glimpl_texture_parameter_v(SGL_CMD_TEXTUREPARAMETERIIV, texture, pname, params);
}

void glTextureParameterIuiv(GLuint texture, GLenum pname, const GLuint* params)
{
    glimpl_texture_parameter_v(SGL_CMD_TEXTUREPARAMETERIUIV, texture, pname, params);
}

void glTextureParameteriv(GLuint texture, GLenum pname, const GLint* param)
{
    glimpl_texture_parameter_v(SGL_CMD_TEXTUREPARAMETERIV, texture, pname, param);
}

void glGetTextureImage(GLuint texture, GLint level, GLenum format, GLenum type, GLsizei bufSize, void* pixels)
{
    /*
     * stub, refer to glGetTexImage
     */
    STUB();
}

void glGetCompressedTextureImage(GLuint texture, GLint level, GLsizei bufSize, void* pixels)
{
    /*
     * stub, refer to glGetCompressedTexImage
     */
    STUB();
}

void glGetTextureLevelParameterfv(GLuint texture, GLint level, GLenum pname, GLfloat* params)
{
    glimpl_get_texture_level_parameter(SGL_CMD_GETTEXTURELEVELPARAMETERFV, texture, level, pname, params);
}

void glGetTextureLevelParameteriv(GLuint texture, GLint level, GLenum pname, GLint* params)
{
    glimpl_get_texture_level_parameter(SGL_CMD_GETTEXTURELEVELPARAMETERIV, texture, level, pname, params);
}

void glGetTextureParameterfv(GLuint texture, GLenum pname, GLfloat* params)
{
    glimpl_get_texture_parameter(SGL_CMD_GETTEXTUREPARAMETERFV, texture, pname, params);
}

void glGetTextureParameterIiv(GLuint texture, GLenum pname, GLint* params)
{
    glimpl_get_texture_parameter(SGL_CMD_GETTEXTUREPARAMETERIIV, texture, pname, params);
}

void glGetTextureParameterIuiv(GLuint texture, GLenum pname, GLuint* params)
{
    glimpl_get_texture_parameter(SGL_CMD_GETTEXTUREPARAMETERIUIV, texture, pname, params);
}

void glGetTextureParameteriv(GLuint texture, GLenum pname, GLint* params)
{
    glimpl_get_texture_parameter(SGL_CMD_GETTEXTUREPARAMETERIV, texture, pname, params);
}

void glCreateVertexArrays(GLsizei n, GLuint* arrays)
{
    GLuint *p = arrays;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_CREATEVERTEXARRAYS);
        // pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glVertexArrayVertexBuffers(GLuint vaobj, GLuint first, GLsizei count, const GLuint* buffers, const GLintptr* offsets, const GLsizei* strides)
{
    pb_push(SGL_CMD_VERTEXARRAYVERTEXBUFFERS);
    pb_push(vaobj);
    pb_push(first);
    pb_push(count);
    for (int i = 0; i < count; i++) {
        pb_push(buffers[i]);
        pb_push(offsets[i]);
        pb_push(strides[i]);
    }
}

void glGetVertexArrayiv(GLuint vaobj, GLenum pname, GLint* param)
{
    pb_push(SGL_CMD_GETVERTEXARRAYIV);
    pb_push(vaobj);
    pb_push(pname);
    
    glimpl_submit();
    *param = pb_read(SGL_OFFSET_REGISTER_RETVAL_V);
}

void glGetVertexArrayIndexediv(GLuint vaobj, GLuint index, GLenum pname, GLint* param)
{
    pb_push(SGL_CMD_GETVERTEXARRAYINDEXEDIV);
    pb_push(vaobj);
    pb_push(index);
    pb_push(pname);
    
    glimpl_submit();
    *param = pb_read(SGL_OFFSET_REGISTER_RETVAL_V);
}

void glGetVertexArrayIndexed64iv(GLuint vaobj, GLuint index, GLenum pname, GLint64* param)
{
    pb_push(SGL_CMD_GETVERTEXARRAYINDEXED64IV);
    pb_push(vaobj);
    pb_push(index);
    pb_push(pname);
    
    glimpl_submit();
    *param = pb_read64(SGL_OFFSET_REGISTER_RETVAL_V);
}

void glCreateSamplers(GLsizei n, GLuint* samplers)
{
    GLuint *p = samplers;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_CREATESAMPLERS);
        // pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glCreateProgramPipelines(GLsizei n, GLuint* pipelines)
{
    GLuint *p = pipelines;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_CREATEPROGRAMPIPELINES);
        // pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glCreateQueries(GLenum target, GLsizei n, GLuint* ids)
{
    GLuint *p = ids;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_CREATEQUERIES);
        pb_push(target);
        // pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGetTextureSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, GLsizei bufSize, void* pixels)
{
    /* 
     * stub
     */
}

void glGetCompressedTextureSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLsizei bufSize, void* pixels)
{
    /* 
     * stub
     */
}

void glGetnCompressedTexImage(GLenum target, GLint lod, GLsizei bufSize, void* pixels)
{
    glGetCompressedTexImage(target, lod, pixels);
}

void glGetnTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLsizei bufSize, void* pixels)
{
    glGetTexImage(target, level, format, type, pixels);
}

void glGetnUniformdv(GLuint program, GLint location, GLsizei bufSize, GLdouble* params)
{
    glGetUniformdv(program, location, params);
}

void glGetnUniformfv(GLuint program, GLint location, GLsizei bufSize, GLfloat* params)
{
    glGetUniformfv(program, location, params);
}

void glGetnUniformiv(GLuint program, GLint location, GLsizei bufSize, GLint* params)
{
    glGetUniformiv(program, location, params);
}

void glGetnUniformuiv(GLuint program, GLint location, GLsizei bufSize, GLuint* params)
{
    glGetUniformuiv(program, location, params);
}

void glReadnPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei bufSize, void* data)
{
    glReadPixels(x, y, width, height, format, type, data);
}

void glGetnMapdv(GLenum target, GLenum query, GLsizei bufSize, GLdouble* v)
{
    glGetMapdv(target, query, v);
}

void glGetnMapfv(GLenum target, GLenum query, GLsizei bufSize, GLfloat* v)
{
    glGetMapfv(target, query, v);
}

void glGetnMapiv(GLenum target, GLenum query, GLsizei bufSize, GLint* v)
{
    glGetMapiv(target, query, v);
}

void glGetnPixelMapfv(GLenum map, GLsizei bufSize, GLfloat* values)
{
    glGetPixelMapfv(map, values);
}

void glGetnPixelMapuiv(GLenum map, GLsizei bufSize, GLuint* values)
{
    glGetPixelMapuiv(map, values);
}

void glGetnPixelMapusv(GLenum map, GLsizei bufSize, GLushort* values)
{
    glGetPixelMapusv(map, values);
}

void glGetnPolygonStipple(GLsizei bufSize, GLubyte* pattern)
{
    glGetPolygonStipple(pattern);
}

void glGetnColorTable(GLenum target, GLenum format, GLenum type, GLsizei bufSize, void* table)
{
    STUB();
}

void glGetnConvolutionFilter(GLenum target, GLenum format, GLenum type, GLsizei bufSize, void* image)
{
    STUB();
}

void glGetnSeparableFilter(GLenum target, GLenum format, GLenum type, GLsizei rowBufSize, void* row, GLsizei columnBufSize, void* column, void* span)
{
    STUB();
}

void glGetnHistogram(GLenum target, GLboolean reset, GLenum format, GLenum type, GLsizei bufSize, void* values)
{
    STUB();
}

void glGetnMinmax(GLenum target, GLboolean reset, GLenum format, GLenum type, GLsizei bufSize, void* values)
{
    STUB();
}

void glAttachObjectARB(GLhandleARB containerObj, GLhandleARB obj)
{
    pb_push(SGL_CMD_ATTACHOBJECTARB);
    pb_push(containerObj);
    pb_push(obj);
}

void glBindAttribLocationARB(GLhandleARB programObj, GLuint index, const GLcharARB* name)
{
    pb_push(SGL_CMD_BINDATTRIBLOCATIONARB);
    pb_push(programObj);
    pb_push(index);
    push_string(name);
}

void glBindBufferARB(GLenum target, GLuint buffer)
{
    pb_push(SGL_CMD_BINDBUFFERARB);
    pb_push(target);
    pb_push(buffer);
}

void glBindProgramARB(GLenum target, GLuint program)
{
    pb_push(SGL_CMD_BINDPROGRAMARB);
    pb_push(target);
    pb_push(program);
}

void glBufferDataARB(GLenum target, GLsizeiptrARB size, const void* data, GLenum usage)
{
    glimpl_buffer_store_data(SGL_CMD_BUFFERDATAARB, target, size, data, usage);
}

void glBufferSubDataARB(GLenum target, GLintptr offset, GLsizeiptr size, const void* data)
{
    glimpl_buffer_subdata(SGL_CMD_BUFFERSUBDATAARB, target, offset, size, data);
}

void glCompileShaderARB(GLhandleARB shaderObj)
{
    pb_push(SGL_CMD_COMPILESHADERARB);
    pb_push(shaderObj);
}

GLhandleARB glCreateProgramObjectARB(void)
{
    pb_push(SGL_CMD_CREATEPROGRAMOBJECTARB);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

GLhandleARB glCreateShaderObjectARB(GLenum shaderType)
{
    pb_push(SGL_CMD_CREATESHADEROBJECTARB);
    pb_push(shaderType);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glDeleteBuffersARB(GLsizei n, const GLuint* buffers)
{
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_DELETEBUFFERSARB);
        pb_push(buffers[i]);
    }
}

void glDeleteObjectARB(GLhandleARB obj)
{
    pb_push(SGL_CMD_DELETEOBJECTARB);
    pb_push(obj);
}

void glDeleteProgramsARB(GLsizei n, const GLuint* programs)
{
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_DELETEPROGRAMSARB);
        pb_push(programs[i]);
    }
}

void glDeleteQueriesARB(GLsizei n, const GLuint* ids)
{
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_DELETEQUERIESARB);
        pb_push(ids[i]);
    }
}

void glDetachObjectARB(GLhandleARB containerObj, GLhandleARB attachedObj)
{
    pb_push(SGL_CMD_DETACHOBJECTARB);
    pb_push(containerObj);
    pb_push(attachedObj);
}

// void glGenBuffersARB(GLsizei n, GLuint* buffers)
// {
//     glGenBuffers(n, buffers);
// }

void glGenBuffersARB(GLsizei n, GLuint* buffers)
{
    GLuint *p = buffers;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_GENBUFFERSARB);
        // pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGenProgramsARB(GLsizei n, GLuint* programs)
{
    GLuint *p = programs;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_GENPROGRAMSARB);
        // pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGenQueriesARB(GLsizei n, GLuint* ids)
{
    GLuint *p = ids;

    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_GENQUERIESARB);
        // pb_push(1);

        glimpl_submit();
        *p++ = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGetInfoLogARB(GLhandleARB obj, GLsizei maxLength, GLsizei* length, GLcharARB* infoLog)
{
    pb_push(SGL_CMD_GETINFOLOGARB);
    pb_push(obj);
    pb_push(maxLength);
    
    glimpl_submit();

    uintptr_t ptr = (uintptr_t)pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V);
    GLsizei length_notptr;

    memcpy(&length_notptr, (void*)(ptr + 0), sizeof(*length));
    if (length != NULL)
        *length = length_notptr;

    memcpy(infoLog, (void*)(ptr + sizeof(*length)), length_notptr);
}

void glGetProgramivARB(GLenum target, GLenum pname, GLint* params)
{
    pb_push(SGL_CMD_GETPROGRAMIVARB);
    pb_push(target);
    pb_push(pname);

    glimpl_submit();
    *params = pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

GLint glGetUniformLocationARB(GLhandleARB programObj, const GLcharARB* name)
{
    pb_push(SGL_CMD_GETUNIFORMLOCATIONARB);
    pb_push(programObj);
    push_string(name);

    glimpl_submit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glLinkProgramARB(GLhandleARB programObj)
{
    pb_push(SGL_CMD_LINKPROGRAMARB);
    pb_push(programObj);
}

void* glMapBufferARB(GLenum target, GLenum access)
{
    return glimpl_map_buffer_range(SGL_CMD_MAPBUFFERARB, target, 0, 0, access);
}

void glProgramStringARB(GLenum target, GLenum format, GLsizei len, const void* string) 
{
    pb_push(SGL_CMD_PROGRAMSTRINGARB);
    pb_push(target);
    pb_push(format);
    pb_push(len);
    push_string(string);
}

void glShaderSourceARB(GLhandleARB shaderObj, GLsizei count, const GLcharARB* *string, const GLint* length)
{
    pb_push(SGL_CMD_SHADERSOURCEARB);
    pb_push(shaderObj);
    pb_push(count);

    for (int i = 0; i < count; i++)
        push_string(string[i]);

    glimpl_submit();
}

void glUniform1iARB(GLint location, GLint v0)
{
    pb_push(SGL_CMD_UNIFORM1IARB);
    pb_push(location);
    pb_push(v0);
}

void glProgramEnvParameters4fvEXT(GLenum target, GLuint index, GLsizei count, const GLfloat* params)
{
    pb_push(SGL_CMD_PROGRAMENVPARAMETERS4FVEXT);
    pb_push(target);
    pb_push(index);
    pb_push(count);
    pb_memcpy(params, count * 4 * sizeof(float));
}

void glColorMaskIndexedEXT(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    pb_push(SGL_CMD_COLORMASKINDEXEDEXT);
    pb_push(index);
    pb_push(r);
    pb_push(g);
    pb_push(b);
    pb_push(a);
}

void glEnableIndexedEXT(GLenum target, GLuint index)
{
    pb_push(SGL_CMD_ENABLEINDEXEDEXT);
    pb_push(target);
    pb_push(index);
}

void glDisableIndexedEXT(GLenum target, GLuint index)
{
    pb_push(SGL_CMD_DISABLEINDEXEDEXT);
    pb_push(target);
    pb_push(index);
}

void glGetBooleanIndexedvEXT(GLenum target, GLuint index, GLboolean* data)
{
    pb_push(SGL_CMD_GETBOOLEANINDEXEDVEXT);
    pb_push(target);
    pb_push(index);

    glimpl_submit();
    *data = *(GLboolean*)pb_ptr(SGL_OFFSET_REGISTER_RETVAL);
}

void glActiveTextureARB(GLenum texture)
{
    pb_push(SGL_CMD_ACTIVETEXTUREARB);
    pb_push(texture);
}

void glMultiTexCoord2fARB(GLenum target, GLfloat s, GLfloat t)
{
    pb_push(SGL_CMD_MULTITEXCOORD2FARB);
    pb_push(target);
    pb_pushf(s);
    pb_pushf(t);
}

#ifdef _WIN32

static const GLCLTPROCTABLE cpt =
{
    OPENGL_VERSION_110_ENTRIES,
    {
        &glNewList,
        &glEndList,
        &glCallList,
        &glCallLists,
        &glDeleteLists,
        &glGenLists,
        &glListBase,
        &glBegin,
        &glBitmap,
        &glColor3b,
        &glColor3bv,
        &glColor3d,
        &glColor3dv,
        &glColor3f,
        &glColor3fv,
        &glColor3i,
        &glColor3iv,
        &glColor3s,
        &glColor3sv,
        &glColor3ub,
        &glColor3ubv,
        &glColor3ui,
        &glColor3uiv,
        &glColor3us,
        &glColor3usv,
        &glColor4b,
        &glColor4bv,
        &glColor4d,
        &glColor4dv,
        &glColor4f,
        &glColor4fv,
        &glColor4i,
        &glColor4iv,
        &glColor4s,
        &glColor4sv,
        &glColor4ub,
        &glColor4ubv,
        &glColor4ui,
        &glColor4uiv,
        &glColor4us,
        &glColor4usv,
        &glEdgeFlag,
        &glEdgeFlagv,
        &glEnd,
        &glIndexd,
        &glIndexdv,
        &glIndexf,
        &glIndexfv,
        &glIndexi,
        &glIndexiv,
        &glIndexs,
        &glIndexsv,
        &glNormal3b,
        &glNormal3bv,
        &glNormal3d,
        &glNormal3dv,
        &glNormal3f,
        &glNormal3fv,
        &glNormal3i,
        &glNormal3iv,
        &glNormal3s,
        &glNormal3sv,
        &glRasterPos2d,
        &glRasterPos2dv,
        &glRasterPos2f,
        &glRasterPos2fv,
        &glRasterPos2i,
        &glRasterPos2iv,
        &glRasterPos2s,
        &glRasterPos2sv,
        &glRasterPos3d,
        &glRasterPos3dv,
        &glRasterPos3f,
        &glRasterPos3fv,
        &glRasterPos3i,
        &glRasterPos3iv,
        &glRasterPos3s,
        &glRasterPos3sv,
        &glRasterPos4d,
        &glRasterPos4dv,
        &glRasterPos4f,
        &glRasterPos4fv,
        &glRasterPos4i,
        &glRasterPos4iv,
        &glRasterPos4s,
        &glRasterPos4sv,
        &glRectd,
        &glRectdv,
        &glRectf,
        &glRectfv,
        &glRecti,
        &glRectiv,
        &glRects,
        &glRectsv,
        &glTexCoord1d,
        &glTexCoord1dv,
        &glTexCoord1f,
        &glTexCoord1fv,
        &glTexCoord1i,
        &glTexCoord1iv,
        &glTexCoord1s,
        &glTexCoord1sv,
        &glTexCoord2d,
        &glTexCoord2dv,
        &glTexCoord2f,
        &glTexCoord2fv,
        &glTexCoord2i,
        &glTexCoord2iv,
        &glTexCoord2s,
        &glTexCoord2sv,
        &glTexCoord3d,
        &glTexCoord3dv,
        &glTexCoord3f,
        &glTexCoord3fv,
        &glTexCoord3i,
        &glTexCoord3iv,
        &glTexCoord3s,
        &glTexCoord3sv,
        &glTexCoord4d,
        &glTexCoord4dv,
        &glTexCoord4f,
        &glTexCoord4fv,
        &glTexCoord4i,
        &glTexCoord4iv,
        &glTexCoord4s,
        &glTexCoord4sv,
        &glVertex2d,
        &glVertex2dv,
        &glVertex2f,
        &glVertex2fv,
        &glVertex2i,
        &glVertex2iv,
        &glVertex2s,
        &glVertex2sv,
        &glVertex3d,
        &glVertex3dv,
        &glVertex3f,
        &glVertex3fv,
        &glVertex3i,
        &glVertex3iv,
        &glVertex3s,
        &glVertex3sv,
        &glVertex4d,
        &glVertex4dv,
        &glVertex4f,
        &glVertex4fv,
        &glVertex4i,
        &glVertex4iv,
        &glVertex4s,
        &glVertex4sv,
        &glClipPlane,
        &glColorMaterial,
        &glCullFace,
        &glFogf,
        &glFogfv,
        &glFogi,
        &glFogiv,
        &glFrontFace,
        &glHint,
        &glLightf,
        &glLightfv,
        &glLighti,
        &glLightiv,
        &glLightModelf,
        &glLightModelfv,
        &glLightModeli,
        &glLightModeliv,
        &glLineStipple,
        &glLineWidth,
        &glMaterialf,
        &glMaterialfv,
        &glMateriali,
        &glMaterialiv,
        &glPointSize,
        &glPolygonMode,
        &glPolygonStipple,
        &glScissor,
        &glShadeModel,
        &glTexParameterf,
        &glTexParameterfv,
        &glTexParameteri,
        &glTexParameteriv,
        &glTexImage1D,
        &glTexImage2D,
        &glTexEnvf,
        &glTexEnvfv,
        &glTexEnvi,
        &glTexEnviv,
        &glTexGend,
        &glTexGendv,
        &glTexGenf,
        &glTexGenfv,
        &glTexGeni,
        &glTexGeniv,
        &glFeedbackBuffer,
        &glSelectBuffer,
        &glRenderMode,
        &glInitNames,
        &glLoadName,
        &glPassThrough,
        &glPopName,
        &glPushName,
        &glDrawBuffer,
        &glClear,
        &glClearAccum,
        &glClearIndex,
        &glClearColor,
        &glClearStencil,
        &glClearDepth,
        &glStencilMask,
        &glColorMask,
        &glDepthMask,
        &glIndexMask,
        &glAccum,
        &glDisable,
        &glEnable,
        &glFinish,
        &glFlush,
        &glPopAttrib,
        &glPushAttrib,
        &glMap1d,
        &glMap1f,
        &glMap2d,
        &glMap2f,
        &glMapGrid1d,
        &glMapGrid1f,
        &glMapGrid2d,
        &glMapGrid2f,
        &glEvalCoord1d,
        &glEvalCoord1dv,
        &glEvalCoord1f,
        &glEvalCoord1fv,
        &glEvalCoord2d,
        &glEvalCoord2dv,
        &glEvalCoord2f,
        &glEvalCoord2fv,
        &glEvalMesh1,
        &glEvalPoint1,
        &glEvalMesh2,
        &glEvalPoint2,
        &glAlphaFunc,
        &glBlendFunc,
        &glLogicOp,
        &glStencilFunc,
        &glStencilOp,
        &glDepthFunc,
        &glPixelZoom,
        &glPixelTransferf,
        &glPixelTransferi,
        &glPixelStoref,
        &glPixelStorei,
        &glPixelMapfv,
        &glPixelMapuiv,
        &glPixelMapusv,
        &glReadBuffer,
        &glCopyPixels,
        &glReadPixels,
        &glDrawPixels,
        &glGetBooleanv,
        &glGetClipPlane,
        &glGetDoublev,
        &glGetError,
        &glGetFloatv,
        &glGetIntegerv,
        &glGetLightfv,
        &glGetLightiv,
        &glGetMapdv,
        &glGetMapfv,
        &glGetMapiv,
        &glGetMaterialfv,
        &glGetMaterialiv,
        &glGetPixelMapfv,
        &glGetPixelMapuiv,
        &glGetPixelMapusv,
        &glGetPolygonStipple,
        &glGetString,
        &glGetTexEnvfv,
        &glGetTexEnviv,
        &glGetTexGendv,
        &glGetTexGenfv,
        &glGetTexGeniv,
        &glGetTexImage,
        &glGetTexParameterfv,
        &glGetTexParameteriv,
        &glGetTexLevelParameterfv,
        &glGetTexLevelParameteriv,
        &glIsEnabled,
        &glIsList,
        &glDepthRange,
        &glFrustum,
        &glLoadIdentity,
        &glLoadMatrixf,
        &glLoadMatrixd,
        &glMatrixMode,
        &glMultMatrixf,
        &glMultMatrixd,
        &glOrtho,
        &glPopMatrix,
        &glPushMatrix,
        &glRotated,
        &glRotatef,
        &glScaled,
        &glScalef,
        &glTranslated,
        &glTranslatef,
        &glViewport,
        &glArrayElement,
        &glBindTexture,
        &glColorPointer,
        &glDisableClientState,
        &glDrawArrays,
        &glDrawElements,
        &glEdgeFlagPointer,
        &glEnableClientState,
        &glIndexPointer,
        &glIndexub,
        &glIndexubv,
        &glInterleavedArrays,
        &glNormalPointer,
        &glPolygonOffset,
        &glTexCoordPointer,
        &glVertexPointer,
        &glAreTexturesResident,
        &glCopyTexImage1D,
        &glCopyTexImage2D,
        &glCopyTexSubImage1D,
        &glCopyTexSubImage2D,
        &glDeleteTextures,
        &glGenTextures,
        &glGetPointerv,
        &glIsTexture,
        &glPrioritizeTextures,
        &glTexSubImage1D,
        &glTexSubImage2D,
        &glPopClientAttrib,
        &glPushClientAttrib
    }
};

PGLCLTPROCTABLE glimpl_GetProcTable()
{
    return (PGLCLTPROCTABLE)&cpt;
}

#endif