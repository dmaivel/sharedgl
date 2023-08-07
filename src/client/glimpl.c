#include <client/glimpl.h>
#include <client/memory.h>
#include <client/pb.h>
#include <sharedgl.h>
#include <commongl.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#ifndef _WIN32
#include <sys/mman.h>
#endif

#define GLIMPL_MAX_OBJECTS 256

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

struct gl_normal_pointer {
    GLenum type;
    GLsizei stride;
    const void *pointer;

    bool in_use;
};

struct gl_vertex_attrib_pointer glimpl_vaps[GLIMPL_MAX_OBJECTS];

struct gl_color_tex_vertex_pointer glimpl_color_ptr,
                                   glimpl_tex_coord_ptr,
                                   glimpl_vertex_ptr;
struct gl_normal_pointer           glimpl_normal_ptr;

#define NUM_EXTENSIONS 8
static const char *glimpl_extensions_full = "GL_ARB_framebuffer_object GL_ARB_shading_language_100 GL_ARB_texture_storage GL_ARB_vertex_array_object GL_EXT_bgra GL_EXT_framebuffer_sRGB GL_EXT_paletted_texture GL_EXT_texture_filter_anisotropic";
static const char glimpl_extensions_list[NUM_EXTENSIONS][64] = {
    "GL_ARB_framebuffer_object",
    "GL_ARB_shading_language_100",
    "GL_ARB_texture_storage",
    "GL_ARB_vertex_array_object",
    "GL_EXT_bgra",
    "GL_EXT_framebuffer_sRGB",
    "GL_EXT_paletted_texture",
    "GL_EXT_texture_filter_anisotropic"
};

static int glimpl_major = 1;
static int glimpl_minor = 2;

void glimpl_commit()
{
    pb_push(0); /* processor will stop at zero */
    pb_write(SGL_OFFSET_REGISTER_COMMIT, 1);
    while (pb_read(SGL_OFFSET_REGISTER_COMMIT) == 1); /* to-do: maybe usleep? */
    pb_reset();
}

void glimpl_goodbye()
{
    /*
     * probably not a good idea to commit
     * all commands before sending our
     * goodbye to the server, so instead
     * we should just overwrite the beginning
     * of the push buffer in case there are
     * any commands leftover
     */
    pb_reset();
    pb_push(SGL_CMD_GOODBYE_WORLD);
    pb_push(0);
    glimpl_commit();

#ifdef _WIN32
    pb_unset();
#endif
}

void glimpl_report(int width, int height)
{
    pb_push(SGL_CMD_REPORT_DIMS);
    pb_push(width);
    pb_push(height);
    glimpl_commit();
}

void glimpl_swap_buffers(int width, int height, int vflip, int format)
{
    pb_push(SGL_CMD_REQUEST_FRAMEBUFFER);
    pb_push(width);
    pb_push(height);
    pb_push(vflip);
    pb_push(format);
    glimpl_commit();
}

void *glimpl_fb_address()
{
    return pb_ptr(pb_read(SGL_OFFSET_REGISTER_FBSTART));
}

void glimpl_init()
{
#ifndef _WIN32
    int fd = shm_open(SGL_SHARED_MEMORY_NAME, O_RDWR, S_IRWXU);
    if (fd == -1)
        fd = sgl_detect_memory("/dev/sharedgl");
    if (fd == -1) {
        fprintf(stderr, "glimpl_init: failed to find memory\n");
        exit(1);
    }

    pb_set(fd);
#else
    pb_set();
#endif
    pb_reset();

    char *gl_version_override = getenv("GL_VERSION_OVERRIDE");

    glimpl_major = gl_version_override ? gl_version_override[0] - '0' : pb_read(SGL_OFFSET_REGISTER_GLMAJ);
    glimpl_minor = gl_version_override ? gl_version_override[2] - '0' : pb_read(SGL_OFFSET_REGISTER_GLMIN);

    pb_push(SGL_CMD_CREATE_CONTEXT);
    glimpl_commit();
}

static struct gl_vertex_attrib_pointer *glimpl_get_enabled_vap()
{
    for (int i = 0; i < GLIMPL_MAX_OBJECTS; i++)
        if (glimpl_vaps[i].enabled)
            return &glimpl_vaps[i];

    /* dumb fallback */
    return &glimpl_vaps[0];
}

static void glimpl_upload_texture(GLsizei width, GLsizei height, GLenum format, const void* pixels)
{
    if (pixels == NULL) {
        pb_push(SGL_CMD_VP_NULL);
        return;
    }

    switch (format) {
    case GL_BGR:
    case GL_RGB: {
        unsigned char *p = (void*)pixels;
        pb_push(SGL_CMD_VP_UPLOAD);
        pb_push(width * height);
        for (int i = 0; i < width * height; i++)
            pb_push(*p++);
        break;
    }
    case GL_RGB8: {
        unsigned int *p = (void*)pixels;
        int size = width * height * 3;
        int rem = size % 4;

        pb_push(SGL_CMD_VP_UPLOAD);
        pb_push(size);

        pb_memcpy((void*)pixels, size, 0);

        break;
    }
    case GL_RGBA8:
    case GL_RGBA:
    case GL_BGRA: {
        unsigned int *p = (void*)pixels;
        pb_push(SGL_CMD_VP_UPLOAD);
        pb_push(width * height);
        for (int i = 0; i < width * height; i++)
            pb_push(*p++);
        break;
    }
    default:
        printf("glimpl_upload_texture: unknown format: %x\n", format);
        break;
    }
}

static void push_string(const char *s)
{
    int len = strlen(s);
    int last = 0;
    for (int j = 0; j < len / 4; j++) {
        pb_push(*(int*)&s[j * 4]);
        last = *(int*)&s[j * 4];
    }

    int rem = len % 4;
    int shift = 0;
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

void glAttachShader(GLuint program, GLuint shader)
{
    pb_push(SGL_CMD_ATTACHSHADER);
    pb_push(program);
    pb_push(shader);
}

void glBegin(GLenum mode) 
{
    // glimpl_commit();
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
    // glimpl_commit();

    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count); /* could be very bad mistake */
    for (int i = 0; i < count; i++)
        pb_push(buffers[i]);
    
    pb_push(SGL_CMD_BINDBUFFERSBASE);
    pb_push(target);
    pb_push(first);
    pb_push(count);

    // glimpl_commit();
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

    // glimpl_commit();
}

void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
    pb_push(SGL_CMD_BLENDFUNC);
    pb_push(sfactor);
    pb_push(dfactor);
}

void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    glimpl_commit();

    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(size / sizeof(int)); /* could be very bad mistake */
    int *idata = (int*)data;
    for (int i = 0; i < size / sizeof(int); i++)
        pb_push(idata[i]);
    
    pb_push(SGL_CMD_BUFFERDATA);
    pb_push(target);
    pb_push(size);
    pb_push(usage);

    glimpl_commit();
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
    glimpl_commit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

GLuint glCreateShader(GLenum type)
{
    pb_push(SGL_CMD_CREATESHADER);
    pb_push(type);
    glimpl_commit();
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
    struct gl_vertex_attrib_pointer *vap = glimpl_get_enabled_vap();

    if (vap->client_managed) {
        if (vap->ptr > (int*)0x10000) {
            pb_push(SGL_CMD_VP_UPLOAD);
            pb_push(vap->size * count);
            for (int i = 0; i < vap->size * count; i++)
                pb_push(vap->ptr[i]);
        }

        pb_push(SGL_CMD_VERTEXATTRIBPOINTER);
        pb_push(vap->index);
        pb_push(vap->size);
        pb_push(vap->type);
        pb_push(vap->normalized);
        pb_push(vap->stride);
        pb_push((int)((long)vap->ptr & 0x00000000FFFFFFFF));

        pb_push(SGL_CMD_ENABLEVERTEXATTRIBARRAY);
        pb_push(vap->index);
    }
    
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

#define GET_MAX_INDEX(x, y) \
    for (int i = 0; i < count; i++) \
        if (y[i] > x) \
            x = y[i]

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
{
    // glimpl_commit();

    /*
     * to-do: check if mode and type is valid
     */

    if (indices) {
        int max_index = 0;
        switch (type) {
        case GL_UNSIGNED_BYTE: {
            const unsigned char *tindices = indices;
            GET_MAX_INDEX(max_index, tindices);
            break;
        }
        case GL_UNSIGNED_SHORT: {
            const unsigned short *tindices = indices;
            GET_MAX_INDEX(max_index, tindices);
            break;
        }
        case GL_UNSIGNED_INT: {
            const unsigned int *tindices = indices;
            GET_MAX_INDEX(max_index, tindices);
            break;
        }
        }

        max_index++;

        if (glimpl_normal_ptr.in_use) {
            int max_normal = max_index;
            int size = 4;

            switch (mode) {
            case GL_TRIANGLES:
            case GL_TRIANGLE_FAN:
            case GL_TRIANGLE_STRIP:
                max_normal /= 3;
                break;
            case GL_QUADS:
                max_normal /= 4;
                break;
            default:
                break;
            }

            switch (glimpl_normal_ptr.type) {
            case GL_FLOAT:
            case GL_INT:
                size = 4;
                break;
            case GL_BYTE:
                size = 1;
                break;
            case GL_SHORT:
                size = 2;
                break;
            case GL_DOUBLE:
                size = 8;
                break;
            }

            pb_push(SGL_CMD_VP_UPLOAD);
            pb_push(max_normal * size);
            const float *fvertices = glimpl_normal_ptr.pointer;

            for (int i = 0; i < max_normal; i++) {
                for (int j = 0; j < size; j++)
                    pb_pushf(*fvertices++);
                for (int j = 0; j < (glimpl_normal_ptr.stride / 4) - size; j++)
                    fvertices++;
            }

            pb_push(SGL_CMD_NORMALPOINTER);
            pb_push(glimpl_normal_ptr.type);
            pb_push(0);
        }

        if (glimpl_color_ptr.in_use) {
            int true_size = glimpl_color_ptr.size;

            if (true_size > 4) {
                switch (true_size) {
                case GL_RGB:
                case GL_BGR:
                    true_size = 3;
                    break;
                case GL_RGBA:
                case GL_BGRA:
                    true_size = 4;
                    break;
                }
            } 

            pb_push(SGL_CMD_VP_UPLOAD);
            pb_push(max_index * glimpl_color_ptr.size);
            const float *color = glimpl_color_ptr.pointer;
            for (int i = 0; i < max_index; i++) {
                for (int j = 0; j < glimpl_color_ptr.size; j++)
                    pb_pushf(*color++);
                for (int j = 0; j < (glimpl_color_ptr.stride / 4) - true_size; j++)
                    color++;
            }

            pb_push(SGL_CMD_COLORPOINTER);
            pb_push(glimpl_color_ptr.size);
            pb_push(glimpl_color_ptr.type);
            pb_push(0);
        }

        if (glimpl_tex_coord_ptr.in_use) {
            pb_push(SGL_CMD_VP_UPLOAD);
            pb_push(max_index * glimpl_tex_coord_ptr.size);
            const float *fvertices = glimpl_tex_coord_ptr.pointer;

            for (int i = 0; i < max_index; i++) {
                for (int j = 0; j < glimpl_tex_coord_ptr.size; j++)
                    pb_pushf(*fvertices++);
                for (int j = 0; j < (glimpl_tex_coord_ptr.stride / 4) - glimpl_tex_coord_ptr.size; j++)
                    fvertices++;
            }

            pb_push(SGL_CMD_TEXCOORDPOINTER);
            pb_push(glimpl_tex_coord_ptr.size);
            pb_push(glimpl_tex_coord_ptr.type);
            pb_push(0);
        }

        if (glimpl_vertex_ptr.in_use) {
            pb_push(SGL_CMD_VP_UPLOAD);
            pb_push(max_index * glimpl_vertex_ptr.size);
            const float *fvertices = glimpl_vertex_ptr.pointer;

            for (int i = 0; i < max_index; i++) {
                for (int j = 0; j < glimpl_vertex_ptr.size; j++)
                    pb_pushf(*fvertices++);
                for (int j = 0; j < (glimpl_vertex_ptr.stride / 4) - glimpl_vertex_ptr.size; j++)
                    fvertices++;
            }

            pb_push(SGL_CMD_VERTEXPOINTER);
            pb_push(glimpl_vertex_ptr.size);
            pb_push(glimpl_vertex_ptr.type);
            pb_push(0);
        }

        /*
         * to-do: pack?
         */
        pb_push(SGL_CMD_VP_UPLOAD);
        pb_push(count);
        switch (type) {
        case GL_UNSIGNED_BYTE: {
            const unsigned char *b = indices;
            for (int i = 0; i < count; i++)
                pb_push(*b++);
            break;
        }
        case GL_UNSIGNED_SHORT: {
            const unsigned short *s = indices;
            for (int i = 0; i < count; i++)
                pb_push(*s++);
            break;
        }
        case GL_UNSIGNED_INT: {
            const unsigned int *u = indices;
            for (int i = 0; i < count; i++)
                pb_push(*u++);
            break;
        }
        }
    }
    
    pb_push(SGL_CMD_DRAWELEMENTS);
    pb_push(mode);
    pb_push(count);
    pb_push(indices != NULL ? GL_UNSIGNED_INT : type); /* to-do: actually use type */
    pb_push(indices != NULL);

    // glimpl_commit();
}

#undef GET_MAX_INDEX

void glEnable(GLenum cap)
{
    pb_push(SGL_CMD_ENABLE);
    pb_push(cap);
}

void glEnableVertexAttribArray(GLuint index)
{
    glimpl_vaps[index].enabled = true;

    if (!glimpl_vaps[index].client_managed) {
        pb_push(SGL_CMD_ENABLEVERTEXATTRIBARRAY);
        pb_push(index);
    }
}

void glEnd(void) 
{
    pb_push(SGL_CMD_END);
    // glimpl_commit();
}

void glEndList(void)
{
    pb_push(SGL_CMD_ENDLIST);
    // glimpl_commit();
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
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_GENBUFFERS);
        pb_push(1);

        glimpl_commit();
        buffers[i] = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGenFramebuffers(GLsizei n, GLuint* framebuffers)
{
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_GENFRAMEBUFFERS);
        pb_push(1);

        glimpl_commit();
        framebuffers[i] = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

GLuint glGenLists(GLsizei range)
{
    pb_push(SGL_CMD_GENLISTS);
    pb_push(range);

    glimpl_commit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glGenQueries(GLsizei n, GLuint* ids)
{
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_GENQUERIES);
        pb_push(1);

        glimpl_commit();
        ids[i] = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGenTextures(GLsizei n, GLuint* textures)
{
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_GENTEXTURES);
        pb_push(1);

        glimpl_commit();
        textures[i] = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGenVertexArrays(GLsizei n, GLuint* arrays)
{
    for (int i = 0; i < n; i++) {
        pb_push(SGL_CMD_GENVERTEXARRAYS);
        pb_push(1);

        glimpl_commit();
        arrays[n] = pb_read(SGL_OFFSET_REGISTER_RETVAL);
    }
}

void glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64 *params)
{
    pb_push(SGL_CMD_GETQUERYOBJECTUI64V);
    pb_push(id);
    pb_push(pname);

    glimpl_commit();
    *params = pb_read64(SGL_OFFSET_REGISTER_RETVAL);
}

void glGetProgramiv(GLuint program, GLenum pname, GLint* params)
{
    /* stub */
    *params = GL_TRUE;
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint* params)
{
    /* stub */
    *params = GL_TRUE;
}

void glGetObjectParameterivARB(void *obj, GLenum pname, GLint* params)
{
    /* stub */
    *params = GL_TRUE;
}

const GLubyte *glGetString(GLenum name)
{
    static char version[16] = "X.X.0 SharedGL";

    if (version[0] == 'X') {
        version[0] = '0' + (char)glimpl_major;
        version[2] = '0' + (char)glimpl_minor;
    }

    switch (name) {
    case GL_VENDOR: return (const GLubyte *)"SharedGL";
    case GL_RENDERER: return (const GLubyte *)"SharedGL Renderer";
    case GL_VERSION: return (const GLubyte *)version;
    case GL_EXTENSIONS: return (const GLubyte *)glimpl_extensions_full;
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"1.20";
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

    glimpl_commit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

GLint glGetAttribLocation(GLuint program, const GLchar* name)
{
    pb_push(SGL_CMD_GETATTRIBLOCATION);
    pb_push(program);
    push_string(name);

    glimpl_commit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

#define GL_GET_MEMCPY_RETVAL(data, type) \
    switch (pname) { \
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
        memcpy(data, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(type) * 2); \
        break; \
    default: \
        memcpy(data, pb_ptr(SGL_OFFSET_REGISTER_RETVAL_V), sizeof(type) * 1); \
        break; \
    }

void glGetFloatv(GLenum pname, GLfloat* data)
{
    pb_push(SGL_CMD_GETFLOATV);
    pb_push(pname);
    glimpl_commit();
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
    glimpl_commit();
    GL_GET_MEMCPY_RETVAL(data, float);
}

#undef GL_GET_MEMCPY_RETVAL

void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* pixels)
{
    /*
     * le epic troll: don't do anything
     */
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
    // glimpl_commit();
}

void glPushMatrix(void)
{
    // glimpl_commit();
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
    glimpl_commit();

    for (int i = 0; i < count; i++) {
        pb_push(SGL_CMD_SHADERSOURCE);
        pb_push(shader);
        push_string(string[i]);
    }

    glimpl_commit();
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels)
{
    glimpl_commit();

    glimpl_upload_texture(width, height, internalformat, pixels);

    pb_push(SGL_CMD_TEXIMAGE2D);
    pb_push(target);
    pb_push(level);
    pb_push(internalformat);
    pb_push(width);
    pb_push(height);
    pb_push(border);
    pb_push(format);
    pb_push(type);
    
    glimpl_commit();
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels)
{
    glimpl_commit();

    glimpl_upload_texture(width, height, format, pixels);

    pb_push(SGL_CMD_TEXSUBIMAGE2D);
    pb_push(target);
    pb_push(level);
    pb_push(xoffset);
    pb_push(yoffset);
    pb_push(width);
    pb_push(height);
    pb_push(format);
    pb_push(type);
    
    glimpl_commit();
}

void glTextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels)
{
    glimpl_commit();

    glimpl_upload_texture(width, height, format, pixels);

    pb_push(SGL_CMD_TEXTURESUBIMAGE2D);
    pb_push(texture);
    pb_push(level);
    pb_push(xoffset);
    pb_push(yoffset);
    pb_push(width);
    pb_push(height);
    pb_push(format);
    pb_push(type);
    
    glimpl_commit();
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
    // glimpl_commit();

    pb_push(SGL_CMD_VP_UPLOAD);
    pb_push(count * 4 * 4);
    for (int i = 0; i < count * 4 * 4; i++)
        pb_pushf(value[i]);

    pb_push(SGL_CMD_UNIFORMMATRIX4FV);
    pb_push(location);
    pb_push(count);
    pb_push(transpose);

    // glimpl_commit();
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
    bool client_managed = pointer > (void*)0x10000;

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
        pb_push((int)((long)pointer & 0x00000000FFFFFFFF));
    }
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    pb_push(SGL_CMD_VIEWPORT);
    pb_push(x);
    pb_push(y);
    pb_push(width);
    pb_push(height);
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
        .in_use = true /* may become an issue, doubtful */
    };
}

void glNormalPointer(GLenum type, GLsizei stride, const void* pointer)
{
    glimpl_normal_ptr = (struct gl_normal_pointer){
        .type = type,
        .stride = stride,
        .pointer = pointer,
        .in_use = true
    };
}

void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* pointer)
{
    glimpl_tex_coord_ptr = (struct gl_color_tex_vertex_pointer){
        .size = size,
        .type = type,
        .stride = stride,
        .pointer = pointer,
        .in_use = true
    };
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void* pointer)
{
    glimpl_vertex_ptr = (struct gl_color_tex_vertex_pointer){
        .size = size,
        .type = type,
        .stride = stride,
        .pointer = pointer,
        .in_use = true
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
    /*
     * le epic troll: don't do anything
     */
}

GLboolean glIsEnabled(GLenum cap)
{
    pb_push(SGL_CMD_ISENABLED);
    pb_push(cap);
    
    glimpl_commit();
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
    pb_push(SGL_CMD_TEXIMAGE2D);
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

    glimpl_commit();
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

    glimpl_commit();
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

    glimpl_commit();
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
        glimpl_tex_coord_ptr.in_use = false;
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
        glimpl_tex_coord_ptr.in_use = true;
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
    pb_push(SGL_CMD_TEXIMAGE3D);
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

    glimpl_commit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

GLboolean glIsBuffer(GLuint buffer)
{
    pb_push(SGL_CMD_ISBUFFER);
    pb_push(buffer);

    glimpl_commit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

GLboolean glUnmapBuffer(GLenum target)
{
    pb_push(SGL_CMD_UNMAPBUFFER);
    pb_push(target);

    glimpl_commit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
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

    if (!glimpl_vaps[index].client_managed) {
        pb_push(SGL_CMD_DISABLEVERTEXATTRIBARRAY);
        pb_push(index);
    }
}

GLboolean glIsProgram(GLuint program)
{
    pb_push(SGL_CMD_ISPROGRAM);
    pb_push(program);

    glimpl_commit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

GLboolean glIsShader(GLuint shader)
{
    pb_push(SGL_CMD_ISSHADER);
    pb_push(shader);

    glimpl_commit();
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

    glimpl_commit();
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

    glimpl_commit();
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

    glimpl_commit();
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

    glimpl_commit();
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
    pb_push(SGL_CMD_FLUSHMAPPEDBUFFERRANGE);
    pb_push(target);
    pb_push(offset);
    pb_push(length);
}

GLboolean glIsVertexArray(GLuint array)
{
    pb_push(SGL_CMD_ISVERTEXARRAY);
    pb_push(array);

    glimpl_commit();
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

    glimpl_commit();
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

    glimpl_commit();
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

    glimpl_commit();
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
    pb_push(SGL_CMD_UNMAPNAMEDBUFFER);
    pb_push(buffer);

    glimpl_commit();
    return pb_read(SGL_OFFSET_REGISTER_RETVAL);
}

void glFlushMappedNamedBufferRange(GLuint buffer, GLintptr offset, GLsizeiptr length)
{
    pb_push(SGL_CMD_FLUSHMAPPEDNAMEDBUFFERRANGE);
    pb_push(buffer);
    pb_push(offset);
    pb_push(length);
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

    glimpl_commit();
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

    glimpl_commit();
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