#define SHAREDGL_HOST
#define SGL_STRING_TABLE
#include <sharedgl.h>
#include <server/context.h>
#include <stdbool.h>
#include <unistd.h>
#include <server/dynarr.h>

#define TIMEOUT_CYCLES 100000

#define ADVANCE_PAST_STRING() \
    while (!(((*pb) & 0xFF) == 0 || ((*pb >> 8) & 0xFF) == 0 || ((*pb >> 16) & 0xFF) == 0 || ((*pb >> 24) & 0xFF) == 0)) \
        pb++; \
    if ((((*pb) & 0xFF) == 0 || ((*pb >> 8) & 0xFF) == 0 || ((*pb >> 16) & 0xFF) == 0 || ((*pb >> 24) & 0xFF) == 0)) \
        pb++;

struct sgl_connection {
    struct sgl_connection *next;

    int id;
    struct sgl_host_context *ctx;
};

static struct sgl_connection *connections = NULL;

static bool match_connection(void *elem, void *data)
{
    struct sgl_connection *con = elem;
    int id = (long)data;

    if (con->id == id)
        sgl_context_destroy(con->ctx);

    return con->id == id;
}

static void connection_add(int id)
{
    struct sgl_connection *con = dynarr_alloc((void**)&connections, 0, sizeof(struct sgl_connection));
    con->id = id;
    con->ctx = sgl_context_create();
}

/*
 * bool reset:
 *  - true:     internally reset to the start of the connections list
 *              only used when a client disconnects to ensure we dont
 *              attempt to access a null pointer
 *  - false:    don't do anything fancy internally, just return either
 *              the next client id, the only client, or loop back and
 *              return the first client id
 *
 * this function returns one of two possible values/ranges:
 * = 0: no current connections exist
 * > 0: a valid connection id
 */
static int connection_get(bool reset)
{
    static struct sgl_connection *cur = NULL;
    if (!connections)
        return 0;

    if (reset) {
        cur = NULL;
        return 0;
    }
    
    if (!cur) {
        cur = connections;
        return cur->id;
    }

    cur = cur->next;
    if (!cur)
        cur = connections;

    return cur->id;
}

static void connection_current(int id)
{
    for (struct sgl_connection *con = connections; con; con = con->next)
        if (con->id == id) {
            sgl_set_current(con->ctx);
            return;
        }
}

static void connection_rem(int id)
{
    dynarr_free_element((void**)&connections, 0, match_connection, (void*)((long)id));
    connection_get(true);
}

static bool wait_for_commit(void *p) 
{
    return *(int*)(p + SGL_OFFSET_REGISTER_COMMIT) == 1;
}

void sgl_cmd_processor_start(size_t m, void *p, int major, int minor, int **internal_cmd_ptr)
{
    int width, height;
    sgl_get_max_resolution(&width, &height);

    /*
     * calculate limits
     */
    size_t framebuffer_size = width * height * 4;
    size_t fifo_size = m - SGL_OFFSET_COMMAND_START - framebuffer_size;

    if ((signed)fifo_size < 0) {
        printf("%sfatal%s: framebuffer too big! try increasing memory\n", COLOR(COLOR_ATTR_BOLD COLOR_FG_RED COLOR_BG_NONE), COLOR(COLOR_RESET));
        return;
    }

    printf("%sinfo%s: [%s0x%08lx%s - %s0x%08lx%s] [%s0x%08lx%s - %s0x%08lx%s] [%s0x%08lx%s - %s0x%08lx%s]\n", 
        COLOR(COLOR_ATTR_BOLD COLOR_FG_BLUE COLOR_BG_NONE), COLOR(COLOR_RESET), 
        COLOR(COLOR_ATTR_BOLD COLOR_FG_GREEN COLOR_BG_NONE), (size_t)0, COLOR(COLOR_RESET), 
        COLOR(COLOR_ATTR_BOLD COLOR_FG_GREEN COLOR_BG_NONE), (size_t)SGL_OFFSET_COMMAND_START - 1, COLOR(COLOR_RESET),
        COLOR(COLOR_ATTR_BOLD COLOR_FG_GREEN COLOR_BG_NONE), (size_t)SGL_OFFSET_COMMAND_START, COLOR(COLOR_RESET), 
        COLOR(COLOR_ATTR_BOLD COLOR_FG_GREEN COLOR_BG_NONE), SGL_OFFSET_COMMAND_START + fifo_size - 1, COLOR(COLOR_RESET), 
        COLOR(COLOR_ATTR_BOLD COLOR_FG_GREEN COLOR_BG_NONE), SGL_OFFSET_COMMAND_START + fifo_size, COLOR(COLOR_RESET), 
        COLOR(COLOR_ATTR_BOLD COLOR_FG_GREEN COLOR_BG_NONE), m, COLOR(COLOR_RESET)
    );
    printf("%sinfo%s: %-26s%-26s%-26s\n\n", COLOR(COLOR_ATTR_BOLD COLOR_FG_BLUE COLOR_BG_NONE), COLOR(COLOR_RESET), "registers", "fifo buffer", "framebuffer");

    memset(p + SGL_OFFSET_COMMAND_START, 0, fifo_size);

    int reported_width = -1;
    int reported_height = -1;

    struct sgl_host_context *ctx;
    bool begun = false;

    *(int*)(p + SGL_OFFSET_REGISTER_FBSTART) = SGL_OFFSET_COMMAND_START + fifo_size;
    *(int*)(p + SGL_OFFSET_REGISTER_MEMSIZE) = m;
    *(int*)(p + SGL_OFFSET_REGISTER_GLMAJ) = major;
    *(int*)(p + SGL_OFFSET_REGISTER_GLMIN) = minor;
    *(int*)(p + SGL_OFFSET_REGISTER_CONNECT) = 0;
    *(int*)(p + SGL_OFFSET_REGISTER_CLAIM_ID) = 1;
    *(int*)(p + SGL_OFFSET_REGISTER_READY_HINT) = 0;
    *(int*)(p + SGL_OFFSET_REGISTER_LOCK) = 0;

    void *uploaded = NULL;
    int cmd;

    if (internal_cmd_ptr)
        *internal_cmd_ptr = &cmd;

    while (1) {
        int client_id = 0;
        int timeout = 0;
        
        /*
         * not only wait for a commit from a specific client,
         * but also ensure that the client we are waiting for
         * still exists, in case it didn't notify the server
         * of its exit
         */
        while (!wait_for_commit(p)) {
            int creg = *(int*)(p + SGL_OFFSET_REGISTER_CONNECT);

            /*
             * a client has notified the server of its attempt
             * to connect
             */
            if (creg != 0) {
                /*
                 * add connection to the dynamic array
                 */
                connection_add(creg);

                printf("%sinfo%s: client %s%d%s connected\n", COLOR(COLOR_ATTR_BOLD COLOR_FG_BLUE COLOR_BG_NONE), COLOR(COLOR_RESET), COLOR(COLOR_ATTR_BOLD COLOR_FG_GREEN COLOR_BG_NONE), creg, COLOR(COLOR_RESET));

                /*
                 * prevent the same client from connecting more
                 * than once and break from the loop
                 */
                *(int*)(p + SGL_OFFSET_REGISTER_CONNECT) = 0;
                continue;
            }

            /*
             * some sort of "sync"
             */
            usleep(1);
        }

        client_id = *(int*)(p + SGL_OFFSET_REGISTER_READY_HINT);
        *(int*)(p + SGL_OFFSET_REGISTER_READY_HINT) = 0;

        /*
         * set the current opengl context to the current client
         */
        connection_current(client_id);

        int *pb = p + SGL_OFFSET_COMMAND_START;
        while (*pb != SGL_CMD_INVALID) {
            cmd = *pb;
            // printf("[+] command: %s (%d)\n", *pb < SGL_CMD_MAX ? SGL_CMD_STRING_TABLE[*pb] : "????", *pb); fflush(stdout);
            //if (*pb >= SGL_CMD_MAX)
            //    exit(1);
            switch (*pb++) {
            /*
             * Internal Implementation
             */
            case SGL_CMD_CREATE_CONTEXT:
                break;
            case SGL_CMD_GOODBYE_WORLD: {
                int id = *pb++;
                printf("%sinfo%s: client %s%d%s disconnected\n", COLOR(COLOR_ATTR_BOLD COLOR_FG_BLUE COLOR_BG_NONE), COLOR(COLOR_RESET), COLOR(COLOR_ATTR_BOLD COLOR_FG_GREEN COLOR_BG_NONE), id, COLOR(COLOR_RESET));
                connection_rem(id);
                memset(p + SGL_OFFSET_COMMAND_START, 0, fifo_size);
                // exit(1);
                break;
            }
            case SGL_CMD_REPORT_DIMS: {
                int w = *pb++,
                    h = *pb++;
                glViewport(0, 0, w, h);
                glScissor(0, 0, w, h);
                break;
            }
            case SGL_CMD_REQUEST_FRAMEBUFFER: {
                int w = *pb++,
                    h = *pb++,
                    vflip = *pb++,
                    format = *pb++;

                sgl_read_pixels(w, h, p + SGL_OFFSET_COMMAND_START + fifo_size, vflip, format, (size_t)pb - (size_t)(p + SGL_OFFSET_COMMAND_START));
                break;
            }
            case SGL_CMD_VP_UPLOAD: {
                int c = *pb++;
                uploaded = pb;
                for (int i = 0; i < c; i++)
                    pb++;
                break;
            }
            /* unused currently so no free */
            case SGL_CMD_VP_UPLOAD_STAY: {
                int c = *pb++;
                void *res = pb;
                for (int i = 0; i < c; i++)
                    pb++;
                uploaded = calloc(c, sizeof(int));
                memcpy(uploaded, res, c * sizeof(int));
                break;
            }
            case SGL_CMD_VP_NULL: {
                uploaded = NULL;
                break;
            }
            
            /*
             * OpenGL Implementation
             */
            case SGL_CMD_ATTACHSHADER: {
                int program = *pb++,
                    shader = *pb++;
                glAttachShader(program, shader);
                break;
            }
            case SGL_CMD_BEGIN:
                begun = true;
                glBegin(*pb++);
                break;
            case SGL_CMD_BEGINQUERY: {
                int target = *pb++,
                    id = *pb++;
                glBeginQuery(target, id);
                break;
            }
            case SGL_CMD_BINDBUFFERSBASE: {
                int target = *pb++,
                    first = *pb++,
                    count = *pb++;
                glBindBuffersBase(target, first, count, uploaded);
                break;
            }
            case SGL_CMD_BINDBUFFER: {
                int target = *pb++,
                    buffer = *pb++;
                glBindBuffer(target, buffer);
                break;
            }
            case SGL_CMD_BINDFRAGDATALOCATION: {
                int program = *pb++,
                    color = *pb++;
                glBindFragDataLocation(program, color, (char*)pb);
                ADVANCE_PAST_STRING();
                break;
            }
            case SGL_CMD_BINDVERTEXARRAY:
                glBindVertexArray(*pb++);
                break;
            case SGL_CMD_BITMAP: {
                int width = *pb++,
                    height = *pb++,
                    xorig = *pb++,
                    yorig = *pb++,
                    xmove = *pb++,
                    ymove = *pb++;
                glBitmap(width, height, xorig, yorig, xmove, ymove, uploaded);
                break;
            }
            case SGL_CMD_BLENDFUNC: {
                int sfactor = *pb++,
                    dfactor = *pb++;
                glBlendFunc(sfactor, dfactor);
                break;
            }
            case SGL_CMD_BUFFERDATA: {
                int target = *pb++,
                    size = *pb++,
                    usage = *pb++;
                glBufferData(target, size, uploaded, usage);
                break;
            }
            case SGL_CMD_CALLLIST:
                glCallList(*pb++);
                break;
            case SGL_CMD_CLEAR:
                glClear(*pb++);
                break;
            case SGL_CMD_CLEARCOLOR: {
                float r = *((float*)pb++),
                      g = *((float*)pb++),
                      b = *((float*)pb++),
                      a = *((float*)pb++);
                glClearColor(r, g, b, a);
                break;
            }
            case SGL_CMD_CLIPPLANE: {
                int plane = *pb++;
                float eq[4];
                eq[0] = *((float*)pb++);
                eq[1] = *((float*)pb++);
                eq[2] = *((float*)pb++);
                eq[3] = *((float*)pb++);
                glClipPlanef(plane, eq);
                break;
            }
            case SGL_CMD_COLOR3F: {
                float r = *((float*)pb++),
                      g = *((float*)pb++),
                      b = *((float*)pb++);
                glColor3f(r, g, b);
                break;
            }
            case SGL_CMD_COMPILESHADER:
                glCompileShader(*pb++);
                break;
            case SGL_CMD_CREATEPROGRAM:
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glCreateProgram();
                break;
            case SGL_CMD_CREATESHADER:
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glCreateShader(*pb++);
                break;
            case SGL_CMD_DELETEBUFFERS: {
                unsigned int buffer = *pb++;
                glDeleteBuffers(1, &buffer);
                break;
            }
            case SGL_CMD_DELETETEXTURES: {
                unsigned int texture = *pb++;
                glDeleteTextures(1, &texture);
                break;
            }
            case SGL_CMD_DELETEVERTEXARRAYS: {
                unsigned int arrays = *pb++;
                glDeleteVertexArrays(1, &arrays);
                break;
            }
            case SGL_CMD_DEPTHFUNC:
                glDepthFunc(*pb++);
                break;
            case SGL_CMD_DELETEPROGRAM:
                glDeleteProgram(*pb++);
                break;
            case SGL_CMD_DELETESHADER:
                glDeleteShader(*pb++);
                break;
            case SGL_CMD_DETACHSHADER: {
                int program = *pb++,
                    shader = *pb++;
                glDetachShader(program, shader);
                break;
            }
            case SGL_CMD_DISABLE:
                glDisable(*pb++);
                break;
            case SGL_CMD_DISPATCHCOMPUTE: {
                int x = *pb++,
                    y = *pb++,
                    z = *pb++;
                glDispatchCompute(x, y, z);
                break;
            }
            case SGL_CMD_DRAWARRAYS: {
                int mode = *pb++,
                    first = *pb++,
                    count = *pb++;
                glDrawArrays(mode, first, count);
                break;
            }
            case SGL_CMD_DRAWBUFFER:
                glDrawBuffer(*pb++);
                break;
            case SGL_CMD_DRAWELEMENTS: {
                int mode = *pb++,
                    count = *pb++,
                    type = *pb++,
                    status = *pb++;
                glDrawElements(mode, count, type, status ? uploaded : NULL);
                break;
            }
            case SGL_CMD_ENABLE:
                glEnable(*pb++);
                break;
            case SGL_CMD_ENABLEVERTEXATTRIBARRAY:
                glEnableVertexAttribArray(*pb++);
                break;
            case SGL_CMD_END:
                begun = false;
                glEnd();
                break;
            case SGL_CMD_ENDLIST:
                glEndList();
                break;
            case SGL_CMD_ENDQUERY:
                glEndQuery(*pb++);
                break;
            case SGL_CMD_FRUSTUM: {
                float left = *((float*)pb++),
                      right = *((float*)pb++),
                      bottom = *((float*)pb++),
                      top = *((float*)pb++),
                      near = *((float*)pb++),
                      far = *((float*)pb++);
                glFrustum(left, right, bottom, top, near, far);
                break;
            }
            case SGL_CMD_GENBUFFERS: {
                GLuint res = 0;
                glGenBuffers(*pb++, &res);
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = res;
                break;
            }
            case SGL_CMD_GENFRAMEBUFFERS: {
                GLuint res = 0;
                glGenFramebuffers(*pb++, &res);
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = res;
                break;
            }
            case SGL_CMD_GENLISTS:
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glGenLists(*pb++);
                break;
            case SGL_CMD_GENQUERIES: {
                GLuint res = 0;
                glGenQueries(*pb++, &res);
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = res;
                break;
            }
            case SGL_CMD_GENTEXTURES: {
                GLuint res = 0;
                glGenTextures(*pb++, &res);
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = res;
                break;
            }
            case SGL_CMD_GENVERTEXARRAYS: {
                GLuint res = 0;
                glGenVertexArrays(*pb++, &res);
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = res;
                break;
            }
            case SGL_CMD_GETQUERYOBJECTUI64V: {
                int id = *pb++,
                    pname = *pb++;
                unsigned long res = 0;
                glGetQueryObjectui64v(id, pname, &res);
                *(long*)(p + SGL_OFFSET_REGISTER_RETVAL) = res;
                break;
            }
            case SGL_CMD_GETUNIFORMLOCATION: {
                int program = *pb++;
                char *name = (char*)pb;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glGetUniformLocation(program, name);
                ADVANCE_PAST_STRING();
                break;
            }
            case SGL_CMD_GETATTRIBLOCATION: {
                int program = *pb++;
                char *name = (char*)pb;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glGetAttribLocation(program, name);
                ADVANCE_PAST_STRING();
                break;
            }
            case SGL_CMD_GETFLOATV: {
                float v[16];
                glGetFloatv(*pb++, v);
                memcpy(p + SGL_OFFSET_REGISTER_RETVAL_V, v, sizeof(float) * 16);
                break;
            }
            case SGL_CMD_GETINTEGERV: {
                int v[16];
                glGetIntegerv(*pb++, v);
                memcpy(p + SGL_OFFSET_REGISTER_RETVAL_V, v, sizeof(int) * 16);
                break;
            }
            case SGL_CMD_GETBOOLEANV: {
                unsigned char v[16];
                glGetBooleanv(*pb++, v);
                memcpy(p + SGL_OFFSET_REGISTER_RETVAL_V, v, sizeof(unsigned char) * 16);
                break;
            }
            case SGL_CMD_GETDOUBLEV: {
                double v[16];
                glGetDoublev(*pb++, v);
                memcpy(p + SGL_OFFSET_REGISTER_RETVAL_V, v, sizeof(double) * 16);
                break;
            }
            case SGL_CMD_LIGHTMODELFV: {
                int pname = *pb++;
                float v[4];
                v[0] = *((float*)pb++);
                v[1] = *((float*)pb++);
                v[2] = *((float*)pb++);
                v[3] = *((float*)pb++);
                glLightModelfv(pname, v);
                break;
            }
            case SGL_CMD_LIGHTFV: {
                int light = *pb++,
                    pname = *pb++;
                float v[4];
                v[0] = *((float*)pb++);
                v[1] = *((float*)pb++);
                v[2] = *((float*)pb++);
                v[3] = *((float*)pb++);
                glLightfv(light, pname, v);
                break;
            }
            case SGL_CMD_LINKPROGRAM:
                glLinkProgram(*pb++);
                break;
            case SGL_CMD_LOADIDENTITY:
                glLoadIdentity();
                break;
            case SGL_CMD_MATERIALFV: {
                int face = *pb++,
                    pname = *pb++;
                float v[4];
                v[0] = *((float*)pb++);
                v[1] = *((float*)pb++);
                v[2] = *((float*)pb++);
                v[3] = *((float*)pb++);
                glMaterialfv(face, pname, v);
                break;
            }
            case SGL_CMD_MATRIXMODE:
                glMatrixMode(*pb++);
                break;
            case SGL_CMD_NEWLIST: {
                int list = *pb++,
                    mode = *pb++;
                glNewList(list, mode);
                break;
            }
            case SGL_CMD_NORMAL3F: {
                float nx = *((float*)pb++),
                      ny = *((float*)pb++),
                      nz = *((float*)pb++);
                glNormal3f(nx, ny, nz);
                break;
            }
            case SGL_CMD_POPMATRIX:
                glPopMatrix();
                break;
            case SGL_CMD_PUSHMATRIX:
                glPushMatrix();
                break;
            case SGL_CMD_ROTATEF: {
                float angle = *((float*)pb++),
                      x = *((float*)pb++),
                      y = *((float*)pb++),
                      z = *((float*)pb++);
                glRotatef(angle, x, y, z);
                break;
            }
            case SGL_CMD_SHADEMODEL:
                glShadeModel(*pb++);
                break;
            case SGL_CMD_SHADERSOURCE: {
                int shader = *pb++;
                char *string = (char*)pb;
                glShaderSource(shader, 1, (const char* const*)&string, NULL);
                ADVANCE_PAST_STRING();
                break;
            }
            case SGL_CMD_TEXIMAGE1D: {
                int target = *pb++,
                    level = *pb++,
                    internalformat = *pb++,
                    width = *pb++,
                    border = *pb++,
                    format = *pb++,
                    type = *pb++;
                glTexImage1D(target, level, internalformat, width, border, format, type, uploaded);
                break;
            }
            case SGL_CMD_TEXSUBIMAGE1D: {
                int target = *pb++,
                    level = *pb++,
                    xoffset = *pb++,
                    width = *pb++,
                    format = *pb++,
                    type = *pb++;
                glTexSubImage1D(target, level, xoffset, width, format, type, uploaded);
                break;
            }
            case SGL_CMD_TEXIMAGE3D: {
                int target = *pb++,
                    level = *pb++,
                    internalformat = *pb++,
                    width = *pb++,
                    height = *pb++,
                    depth = *pb++,
                    border = *pb++,
                    format = *pb++,
                    type = *pb++;
                glTexImage3D(target, level, internalformat, width, height, depth, border, format, type, uploaded);
                break;
            }
            case SGL_CMD_TEXSUBIMAGE3D: {
                int target = *pb++,
                    level = *pb++,
                    xoffset = *pb++,
                    yoffset = *pb++,
                    zoffset = *pb++,
                    width = *pb++,
                    height = *pb++,
                    depth = *pb++,
                    format = *pb++,
                    type = *pb++;
                glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, uploaded);
                break;
            }
            case SGL_CMD_TEXIMAGE2D: {
                int target = *pb++,
                    level = *pb++,
                    internalformat = *pb++,
                    width = *pb++,
                    height = *pb++,
                    border = *pb++,
                    format = *pb++,
                    type = *pb++;
                glTexImage2D(target, level, internalformat, width, height, border, format, type, uploaded);
                break;
            }
            case SGL_CMD_TEXSUBIMAGE2D: {
                int target = *pb++,
                    level = *pb++,
                    xoffset = *pb++,
                    yoffset = *pb++,
                    width = *pb++,
                    height = *pb++,
                    format = *pb++,
                    type = *pb++;
                glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, uploaded);
                break;
            }
            case SGL_CMD_TEXTURESUBIMAGE2D: {
                int texture = *pb++,
                    level = *pb++,
                    xoffset = *pb++,
                    yoffset = *pb++,
                    width = *pb++,
                    height = *pb++,
                    format = *pb++,
                    type = *pb++;
                glTextureSubImage2D(texture, level, xoffset, yoffset, width, height, format, type, uploaded);
                break;
            }
            case SGL_CMD_TRANSLATED: /* to-do: don't discard doubles */
            case SGL_CMD_TRANSLATEF: {
                float x = *((float*)pb++),
                      y = *((float*)pb++),
                      z = *((float*)pb++);
                glTranslatef(x, y, z);
                break;
            }
            case SGL_CMD_UNIFORM1F: {
                int location = *pb++;
                float v0 = *((float*)pb++);
                glUniform1f(location, v0);
                break;
            }
            case SGL_CMD_UNIFORMMATRIX4FV: {
                int location = *pb++,
                    count = *pb++,
                    transpose = *pb++;
                glUniformMatrix4fv(location, count, transpose, uploaded);
                break;
            }
            case SGL_CMD_USEPROGRAM:
                glUseProgram(*pb++);
                break;
            case SGL_CMD_VERTEX3F: {
                float x = *((float*)pb++),
                      y = *((float*)pb++),
                      z = *((float*)pb++);
                glVertex3f(x, y, z);
                break;
            }
            case SGL_CMD_VERTEXATTRIBPOINTER: {
                int index = *pb++,
                    size = *pb++,
                    type = *pb++,
                    normalized = *pb++,
                    stride = *pb++,
                    ptr = *pb++;
                glVertexAttribPointer(index, size, type, normalized, stride, (ptr > 0x10000) ? uploaded : (void*)(long)ptr);
                break;
            }
            case SGL_CMD_VIEWPORT: {
                int x = *pb++,
                    y = *pb++,
                    width = *pb++,
                    height = *pb++;
                glViewport(x, y, width, height);
                break;
            }

            case SGL_CMD_LOADMATRIXD:
            case SGL_CMD_LOADMATRIXF: {
                float m[16];
                for (int i = 0; i < 16; i++)
                    m[i] = *((float*)pb++);
                glLoadMatrixf(m);
                break;
            }

            case SGL_CMD_MULTMATRIXD:
            case SGL_CMD_MULTMATRIXF: {
                float m[16];
                for (int i = 0; i < 16; i++)
                    m[i] = *((float*)pb++);
                glMultMatrixf(m);
                break;
            }
            
            case SGL_CMD_COLORPOINTER: {
                int size = *pb++,
                    type = *pb++,
                    stride = *pb++;
                glColorPointer(size, type, stride, uploaded);
                //// printf("glColorPointer(0x%x, 0x%x, %d, [%f, %f, %f, %f, %f, %f, ...]);\n", size, type, stride, ((float*)uploaded)[0], ((float*)uploaded)[1], ((float*)uploaded)[2], ((float*)uploaded)[3], ((float*)uploaded)[4], ((float*)uploaded)[5]);
                break;
            }
            case SGL_CMD_NORMALPOINTER: {
                int type = *pb++,
                    stride = *pb++;
                glNormalPointer(type, stride, uploaded);
                //// printf("glNormalPointer(0x%x, %d, [%f, %f, %f, %f, %f, %f, ...]);\n", type, stride, ((float*)uploaded)[0], ((float*)uploaded)[1], ((float*)uploaded)[2], ((float*)uploaded)[3], ((float*)uploaded)[4], ((float*)uploaded)[5]);
                break;
            }
            case SGL_CMD_TEXCOORDPOINTER: {
                int size = *pb++,
                    type = *pb++,
                    stride = *pb++;
                glTexCoordPointer(size, type, stride, uploaded);
                //// printf("glTexCoordPointer(0x%x, 0x%x, %d, [%f, %f, %f, %f, %f, %f, ...]);\n", size, type, stride, ((float*)uploaded)[0], ((float*)uploaded)[1], ((float*)uploaded)[2], ((float*)uploaded)[3], ((float*)uploaded)[4], ((float*)uploaded)[5]);
                break;
            }
            case SGL_CMD_VERTEXPOINTER: {
                int size = *pb++,
                    type = *pb++,
                    stride = *pb++;
                glVertexPointer(size, type, stride, uploaded);
                //// printf("glVertexPointer(0x%x, 0x%x, %d, [%f, %f, %f, %f, %f, %f, ...]);\n", size, type, stride, ((float*)uploaded)[0], ((float*)uploaded)[1], ((float*)uploaded)[2], ((float*)uploaded)[3], ((float*)uploaded)[4], ((float*)uploaded)[5]);
                break;
            }

            case SGL_CMD_CULLFACE: {
                int mode = *pb++;
                glCullFace(mode);
                break;
            }
            case SGL_CMD_FRONTFACE: {
                int mode = *pb++;
                glFrontFace(mode);
                break;
            }
            case SGL_CMD_HINT: {
                int target = *pb++;
                int mode = *pb++;
                glHint(target, mode);
                break;
            }
            case SGL_CMD_LINEWIDTH: {
                float width = *((float*)pb++);
                glLineWidth(width);
                break;
            }
            case SGL_CMD_POINTSIZE: {
                float size = *((float*)pb++);
                glPointSize(size);
                break;
            }
            case SGL_CMD_POLYGONMODE: {
                int face = *pb++;
                int mode = *pb++;
                glPolygonMode(face, mode);
                break;
            }
            case SGL_CMD_SCISSOR: {
                int x = *pb++;
                int y = *pb++;
                int width = *pb++;
                int height = *pb++;
                glScissor(x, y, width, height);
                break;
            }
            case SGL_CMD_TEXPARAMETERF: {
                int target = *pb++;
                int pname = *pb++;
                float param = *((float*)pb++);
                glTexParameterf(target, pname, param);
                break;
            }
            case SGL_CMD_TEXPARAMETERI: {
                int target = *pb++;
                int pname = *pb++;
                int param = *pb++;
                glTexParameteri(target, pname, param);
                break;
            }
            case SGL_CMD_CLEARSTENCIL: {
                int s = *pb++;
                glClearStencil(s);
                break;
            }
            case SGL_CMD_CLEARDEPTH: {
                float depth = *((float*)pb++);
                glClearDepth(depth);
                //// printf("glClearDepth(%f);\n", depth);
                break;
            }
            case SGL_CMD_STENCILMASK: {
                int mask = *pb++;
                glStencilMask(mask);
                break;
            }
            case SGL_CMD_COLORMASK: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                int alpha = *pb++;
                glColorMask(red, green, blue, alpha);
                break;
            }
            case SGL_CMD_DEPTHMASK: {
                int flag = *pb++;
                glDepthMask(flag);
                break;
            }
            case SGL_CMD_FINISH: {
                glFinish();
                break;
            }
            case SGL_CMD_FLUSH: {
                glFlush();
                break;
            }
            case SGL_CMD_LOGICOP: {
                int opcode = *pb++;
                glLogicOp(opcode);
                break;
            }
            case SGL_CMD_STENCILFUNC: {
                int func = *pb++;
                int ref = *pb++;
                int mask = *pb++;
                glStencilFunc(func, ref, mask);
                break;
            }
            case SGL_CMD_STENCILOP: {
                int fail = *pb++;
                int zfail = *pb++;
                int zpass = *pb++;
                glStencilOp(fail, zfail, zpass);
                break;
            }
            case SGL_CMD_PIXELSTOREF: {
                int pname = *pb++;
                float param = *((float*)pb++);
                glPixelStoref(pname, param);
                break;
            }
            case SGL_CMD_PIXELSTOREI: {
                int pname = *pb++;
                int param = *pb++;
                glPixelStorei(pname, param);
                break;
            }
            case SGL_CMD_READBUFFER: {
                int src = *pb++;
                glReadBuffer(src);
                break;
            }
            case SGL_CMD_ISENABLED: {
                int cap = *pb++;
                glIsEnabled(cap);
                break;
            }
            case SGL_CMD_DEPTHRANGE: {
                float n = *((float*)pb++);
                float f = *((float*)pb++);
                glDepthRange(n, f);
                break;
            }
            case SGL_CMD_DELETELISTS: {
                int list = *pb++;
                int range = *pb++;
                glDeleteLists(list, range);
                break;
            }
            case SGL_CMD_LISTBASE: {
                int base = *pb++;
                glListBase(base);
                break;
            }
            case SGL_CMD_COLOR3B: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                glColor3b(red, green, blue);
                break;
            }
            case SGL_CMD_COLOR3D: {
                float red = *((float*)pb++);
                float green = *((float*)pb++);
                float blue = *((float*)pb++);
                glColor3d(red, green, blue);
                break;
            }
            case SGL_CMD_COLOR3I: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                glColor3i(red, green, blue);
                break;
            }
            case SGL_CMD_COLOR3S: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                glColor3s(red, green, blue);
                break;
            }
            case SGL_CMD_COLOR3UB: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                glColor3ub(red, green, blue);
                break;
            }
            case SGL_CMD_COLOR3UI: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                glColor3ui(red, green, blue);
                break;
            }
            case SGL_CMD_COLOR3US: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                glColor3us(red, green, blue);
                break;
            }
            case SGL_CMD_COLOR4B: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                int alpha = *pb++;
                glColor4b(red, green, blue, alpha);
                break;
            }
            case SGL_CMD_COLOR4D: {
                float red = *((float*)pb++);
                float green = *((float*)pb++);
                float blue = *((float*)pb++);
                float alpha = *((float*)pb++);
                glColor4d(red, green, blue, alpha);
                break;
            }
            case SGL_CMD_COLOR4F: {
                float red = *((float*)pb++);
                float green = *((float*)pb++);
                float blue = *((float*)pb++);
                float alpha = *((float*)pb++);
                glColor4f(red, green, blue, alpha);
                break;
            }
            case SGL_CMD_COLOR4I: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                int alpha = *pb++;
                glColor4i(red, green, blue, alpha);
                break;
            }
            case SGL_CMD_COLOR4S: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                int alpha = *pb++;
                glColor4s(red, green, blue, alpha);
                break;
            }
            case SGL_CMD_COLOR4UB: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                int alpha = *pb++;
                glColor4ub(red, green, blue, alpha);
                break;
            }
            case SGL_CMD_COLOR4UI: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                int alpha = *pb++;
                glColor4ui(red, green, blue, alpha);
                break;
            }
            case SGL_CMD_COLOR4US: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                int alpha = *pb++;
                glColor4us(red, green, blue, alpha);
                break;
            }
            case SGL_CMD_EDGEFLAG: {
                int flag = *pb++;
                glEdgeFlag(flag);
                break;
            }
            case SGL_CMD_INDEXD: {
                float c = *((float*)pb++);
                glIndexd(c);
                break;
            }
            case SGL_CMD_INDEXF: {
                float c = *((float*)pb++);
                glIndexf(c);
                break;
            }
            case SGL_CMD_INDEXI: {
                int c = *pb++;
                glIndexi(c);
                break;
            }
            case SGL_CMD_INDEXS: {
                int c = *pb++;
                glIndexs(c);
                break;
            }
            case SGL_CMD_NORMAL3B: {
                int nx = *pb++;
                int ny = *pb++;
                int nz = *pb++;
                glNormal3b(nx, ny, nz);
                break;
            }
            case SGL_CMD_NORMAL3D: {
                float nx = *((float*)pb++);
                float ny = *((float*)pb++);
                float nz = *((float*)pb++);
                glNormal3d(nx, ny, nz);
                break;
            }
            case SGL_CMD_NORMAL3I: {
                int nx = *pb++;
                int ny = *pb++;
                int nz = *pb++;
                glNormal3i(nx, ny, nz);
                break;
            }
            case SGL_CMD_NORMAL3S: {
                int nx = *pb++;
                int ny = *pb++;
                int nz = *pb++;
                glNormal3s(nx, ny, nz);
                break;
            }
            case SGL_CMD_RASTERPOS2D: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                glRasterPos2d(x, y);
                break;
            }
            case SGL_CMD_RASTERPOS2F: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                glRasterPos2f(x, y);
                break;
            }
            case SGL_CMD_RASTERPOS2I: {
                int x = *pb++;
                int y = *pb++;
                glRasterPos2i(x, y);
                break;
            }
            case SGL_CMD_RASTERPOS2S: {
                int x = *pb++;
                int y = *pb++;
                glRasterPos2s(x, y);
                break;
            }
            case SGL_CMD_RASTERPOS3D: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                glRasterPos3d(x, y, z);
                break;
            }
            case SGL_CMD_RASTERPOS3F: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                glRasterPos3f(x, y, z);
                break;
            }
            case SGL_CMD_RASTERPOS3I: {
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                glRasterPos3i(x, y, z);
                break;
            }
            case SGL_CMD_RASTERPOS3S: {
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                glRasterPos3s(x, y, z);
                break;
            }
            case SGL_CMD_RASTERPOS4D: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                float w = *((float*)pb++);
                glRasterPos4d(x, y, z, w);
                break;
            }
            case SGL_CMD_RASTERPOS4F: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                float w = *((float*)pb++);
                glRasterPos4f(x, y, z, w);
                break;
            }
            case SGL_CMD_RASTERPOS4I: {
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                int w = *pb++;
                glRasterPos4i(x, y, z, w);
                break;
            }
            case SGL_CMD_RASTERPOS4S: {
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                int w = *pb++;
                glRasterPos4s(x, y, z, w);
                break;
            }
            case SGL_CMD_RECTD: {
                float x1 = *((float*)pb++);
                float y1 = *((float*)pb++);
                float x2 = *((float*)pb++);
                float y2 = *((float*)pb++);
                glRectd(x1, y1, x2, y2);
                break;
            }
            case SGL_CMD_RECTF: {
                float x1 = *((float*)pb++);
                float y1 = *((float*)pb++);
                float x2 = *((float*)pb++);
                float y2 = *((float*)pb++);
                glRectf(x1, y1, x2, y2);
                break;
            }
            case SGL_CMD_RECTI: {
                int x1 = *pb++;
                int y1 = *pb++;
                int x2 = *pb++;
                int y2 = *pb++;
                glRecti(x1, y1, x2, y2);
                break;
            }
            case SGL_CMD_RECTS: {
                int x1 = *pb++;
                int y1 = *pb++;
                int x2 = *pb++;
                int y2 = *pb++;
                glRects(x1, y1, x2, y2);
                break;
            }
            case SGL_CMD_TEXCOORD1D: {
                float s = *((float*)pb++);
                glTexCoord1d(s);
                break;
            }
            case SGL_CMD_TEXCOORD1F: {
                float s = *((float*)pb++);
                glTexCoord1f(s);
                break;
            }
            case SGL_CMD_TEXCOORD1I: {
                int s = *pb++;
                glTexCoord1i(s);
                break;
            }
            case SGL_CMD_TEXCOORD1S: {
                int s = *pb++;
                glTexCoord1s(s);
                break;
            }
            case SGL_CMD_TEXCOORD2D: {
                float s = *((float*)pb++);
                float t = *((float*)pb++);
                glTexCoord2d(s, t);
                break;
            }
            case SGL_CMD_TEXCOORD2F: {
                float s = *((float*)pb++);
                float t = *((float*)pb++);
                glTexCoord2f(s, t);
                break;
            }
            case SGL_CMD_TEXCOORD2I: {
                int s = *pb++;
                int t = *pb++;
                glTexCoord2i(s, t);
                break;
            }
            case SGL_CMD_TEXCOORD2S: {
                int s = *pb++;
                int t = *pb++;
                glTexCoord2s(s, t);
                break;
            }
            case SGL_CMD_TEXCOORD3D: {
                float s = *((float*)pb++);
                float t = *((float*)pb++);
                float r = *((float*)pb++);
                glTexCoord3d(s, t, r);
                break;
            }
            case SGL_CMD_TEXCOORD3F: {
                float s = *((float*)pb++);
                float t = *((float*)pb++);
                float r = *((float*)pb++);
                glTexCoord3f(s, t, r);
                break;
            }
            case SGL_CMD_TEXCOORD3I: {
                int s = *pb++;
                int t = *pb++;
                int r = *pb++;
                glTexCoord3i(s, t, r);
                break;
            }
            case SGL_CMD_TEXCOORD3S: {
                int s = *pb++;
                int t = *pb++;
                int r = *pb++;
                glTexCoord3s(s, t, r);
                break;
            }
            case SGL_CMD_TEXCOORD4D: {
                float s = *((float*)pb++);
                float t = *((float*)pb++);
                float r = *((float*)pb++);
                float q = *((float*)pb++);
                glTexCoord4d(s, t, r, q);
                break;
            }
            case SGL_CMD_TEXCOORD4F: {
                float s = *((float*)pb++);
                float t = *((float*)pb++);
                float r = *((float*)pb++);
                float q = *((float*)pb++);
                glTexCoord4f(s, t, r, q);
                break;
            }
            case SGL_CMD_TEXCOORD4I: {
                int s = *pb++;
                int t = *pb++;
                int r = *pb++;
                int q = *pb++;
                glTexCoord4i(s, t, r, q);
                break;
            }
            case SGL_CMD_TEXCOORD4S: {
                int s = *pb++;
                int t = *pb++;
                int r = *pb++;
                int q = *pb++;
                glTexCoord4s(s, t, r, q);
                break;
            }
            case SGL_CMD_VERTEX2D: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                glVertex2d(x, y);
                break;
            }
            case SGL_CMD_VERTEX2F: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                glVertex2f(x, y);
                break;
            }
            case SGL_CMD_VERTEX2I: {
                int x = *pb++;
                int y = *pb++;
                glVertex2i(x, y);
                break;
            }
            case SGL_CMD_VERTEX2S: {
                int x = *pb++;
                int y = *pb++;
                glVertex2s(x, y);
                break;
            }
            case SGL_CMD_VERTEX3D: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                glVertex3d(x, y, z);
                break;
            }
            case SGL_CMD_VERTEX3I: {
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                glVertex3i(x, y, z);
                break;
            }
            case SGL_CMD_VERTEX3S: {
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                glVertex3s(x, y, z);
                break;
            }
            case SGL_CMD_VERTEX4D: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                float w = *((float*)pb++);
                glVertex4d(x, y, z, w);
                break;
            }
            case SGL_CMD_VERTEX4F: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                float w = *((float*)pb++);
                glVertex4f(x, y, z, w);
                break;
            }
            case SGL_CMD_VERTEX4I: {
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                int w = *pb++;
                glVertex4i(x, y, z, w);
                break;
            }
            case SGL_CMD_VERTEX4S: {
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                int w = *pb++;
                glVertex4s(x, y, z, w);
                break;
            }
            case SGL_CMD_COLORMATERIAL: {
                int face = *pb++;
                int mode = *pb++;
                glColorMaterial(face, mode);
                break;
            }
            case SGL_CMD_FOGF: {
                int pname = *pb++;
                float param = *((float*)pb++);
                glFogf(pname, param);
                break;
            }
            case SGL_CMD_FOGFV: {
                int pname = *pb++;
                float params[4];
                params[0] = *((float*)pb++);
                params[1] = *((float*)pb++);
                params[2] = *((float*)pb++);
                params[3] = *((float*)pb++);
                glFogfv(pname, params);
                break;
            }
            case SGL_CMD_FOGI: {
                int pname = *pb++;
                int param = *pb++;
                glFogi(pname, param);
                break;
            }
            case SGL_CMD_LIGHTF: {
                int light = *pb++;
                int pname = *pb++;
                float param = *((float*)pb++);
                glLightf(light, pname, param);
                break;
            }
            case SGL_CMD_LIGHTI: {
                int light = *pb++;
                int pname = *pb++;
                int param = *pb++;
                glLighti(light, pname, param);
                break;
            }
            case SGL_CMD_LIGHTMODELF: {
                int pname = *pb++;
                float param = *((float*)pb++);
                glLightModelf(pname, param);
                break;
            }
            case SGL_CMD_LIGHTMODELI: {
                int pname = *pb++;
                int param = *pb++;
                glLightModeli(pname, param);
                break;
            }
            case SGL_CMD_LINESTIPPLE: {
                int factor = *pb++;
                int pattern = *pb++;
                glLineStipple(factor, pattern);
                break;
            }
            case SGL_CMD_MATERIALF: {
                int face = *pb++;
                int pname = *pb++;
                float param = *((float*)pb++);
                glMaterialf(face, pname, param);
                break;
            }
            case SGL_CMD_MATERIALI: {
                int face = *pb++;
                int pname = *pb++;
                int param = *pb++;
                glMateriali(face, pname, param);
                break;
            }
            case SGL_CMD_TEXENVF: {
                int target = *pb++;
                int pname = *pb++;
                float param = *((float*)pb++);
                glTexEnvf(target, pname, param);
                break;
            }
            case SGL_CMD_TEXENVI: {
                int target = *pb++;
                int pname = *pb++;
                int param = *pb++;
                glTexEnvi(target, pname, param);
                break;
            }
            case SGL_CMD_TEXENVFV: {
                int target = *pb++;
                int pname = *pb++;
                float param[4];
                param[0] = *((float*)pb++);
                param[1] = *((float*)pb++);
                param[2] = *((float*)pb++);
                param[3] = *((float*)pb++);
                glTexEnvfv(target, pname, param);
                break;
            }
            case SGL_CMD_TEXENVIV: {
                int target = *pb++;
                int pname = *pb++;
                int param[4];
                param[0] = *pb++;
                param[1] = *pb++;
                param[2] = *pb++;
                param[3] = *pb++;
                glTexEnviv(target, pname, param);
                break;
            }
            case SGL_CMD_TEXGEND: {
                int coord = *pb++;
                int pname = *pb++;
                float param = *((float*)pb++);
                glTexGend(coord, pname, param);
                break;
            }
            case SGL_CMD_TEXGENF: {
                int coord = *pb++;
                int pname = *pb++;
                float param = *((float*)pb++);
                glTexGenf(coord, pname, param);
                break;
            }
            case SGL_CMD_TEXGENI: {
                int coord = *pb++;
                int pname = *pb++;
                int param = *pb++;
                glTexGeni(coord, pname, param);
                break;
            }
            case SGL_CMD_RENDERMODE: {
                int mode = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glRenderMode(mode);
                break;
            }
            case SGL_CMD_INITNAMES: {
                glInitNames();
                break;
            }
            case SGL_CMD_LOADNAME: {
                int name = *pb++;
                glLoadName(name);
                break;
            }
            case SGL_CMD_PASSTHROUGH: {
                float token = *((float*)pb++);
                glPassThrough(token);
                break;
            }
            case SGL_CMD_POPNAME: {
                glPopName();
                break;
            }
            case SGL_CMD_PUSHNAME: {
                int name = *pb++;
                glPushName(name);
                break;
            }
            case SGL_CMD_CLEARACCUM: {
                float red = *((float*)pb++);
                float green = *((float*)pb++);
                float blue = *((float*)pb++);
                float alpha = *((float*)pb++);
                glClearAccum(red, green, blue, alpha);
                break;
            }
            case SGL_CMD_CLEARINDEX: {
                float c = *((float*)pb++);
                glClearIndex(c);
                break;
            }
            case SGL_CMD_INDEXMASK: {
                int mask = *pb++;
                glIndexMask(mask);
                break;
            }
            case SGL_CMD_ACCUM: {
                int op = *pb++;
                float value = *((float*)pb++);
                glAccum(op, value);
                break;
            }
            case SGL_CMD_POPATTRIB: {
                glPopAttrib();
                break;
            }
            case SGL_CMD_PUSHATTRIB: {
                int mask = *pb++;
                glPushAttrib(mask);
                break;
            }
            case SGL_CMD_MAPGRID1D: {
                int un = *pb++;
                float u1 = *((float*)pb++);
                float u2 = *((float*)pb++);
                glMapGrid1d(un, u1, u2);
                break;
            }
            case SGL_CMD_MAPGRID1F: {
                int un = *pb++;
                float u1 = *((float*)pb++);
                float u2 = *((float*)pb++);
                glMapGrid1f(un, u1, u2);
                break;
            }
            case SGL_CMD_MAPGRID2D: {
                int un = *pb++;
                float u1 = *((float*)pb++);
                float u2 = *((float*)pb++);
                int vn = *pb++;
                float v1 = *((float*)pb++);
                float v2 = *((float*)pb++);
                glMapGrid2d(un, u1, u2, vn, v1, v2);
                break;
            }
            case SGL_CMD_MAPGRID2F: {
                int un = *pb++;
                float u1 = *((float*)pb++);
                float u2 = *((float*)pb++);
                int vn = *pb++;
                float v1 = *((float*)pb++);
                float v2 = *((float*)pb++);
                glMapGrid2f(un, u1, u2, vn, v1, v2);
                break;
            }
            case SGL_CMD_EVALCOORD1D: {
                float u = *((float*)pb++);
                glEvalCoord1d(u);
                break;
            }
            case SGL_CMD_EVALCOORD1F: {
                float u = *((float*)pb++);
                glEvalCoord1f(u);
                break;
            }
            case SGL_CMD_EVALCOORD2D: {
                float u = *((float*)pb++);
                float v = *((float*)pb++);
                glEvalCoord2d(u, v);
                break;
            }
            case SGL_CMD_EVALCOORD2F: {
                float u = *((float*)pb++);
                float v = *((float*)pb++);
                glEvalCoord2f(u, v);
                break;
            }
            case SGL_CMD_EVALMESH1: {
                int mode = *pb++;
                int i1 = *pb++;
                int i2 = *pb++;
                glEvalMesh1(mode, i1, i2);
                break;
            }
            case SGL_CMD_EVALPOINT1: {
                int i = *pb++;
                glEvalPoint1(i);
                break;
            }
            case SGL_CMD_EVALMESH2: {
                int mode = *pb++;
                int i1 = *pb++;
                int i2 = *pb++;
                int j1 = *pb++;
                int j2 = *pb++;
                glEvalMesh2(mode, i1, i2, j1, j2);
                break;
            }
            case SGL_CMD_EVALPOINT2: {
                int i = *pb++;
                int j = *pb++;
                glEvalPoint2(i, j);
                break;
            }
            case SGL_CMD_ALPHAFUNC: {
                int func = *pb++;
                float ref = *((float*)pb++);
                glAlphaFunc(func, ref);
                break;
            }
            case SGL_CMD_PIXELZOOM: {
                float xfactor = *((float*)pb++);
                float yfactor = *((float*)pb++);
                glPixelZoom(xfactor, yfactor);
                break;
            }
            case SGL_CMD_PIXELTRANSFERF: {
                int pname = *pb++;
                float param = *((float*)pb++);
                glPixelTransferf(pname, param);
                break;
            }
            case SGL_CMD_PIXELTRANSFERI: {
                int pname = *pb++;
                int param = *pb++;
                glPixelTransferi(pname, param);
                break;
            }
            case SGL_CMD_COPYPIXELS: {
                int x = *pb++;
                int y = *pb++;
                int width = *pb++;
                int height = *pb++;
                int type = *pb++;
                glCopyPixels(x, y, width, height, type);
                break;
            }
            case SGL_CMD_ISLIST: {
                int list = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glIsList(list);
                break;
            }
            case SGL_CMD_ORTHO: {
                float left = *((float*)pb++);
                float right = *((float*)pb++);
                float bottom = *((float*)pb++);
                float top = *((float*)pb++);
                float zNear = *((float*)pb++);
                float zFar = *((float*)pb++);
                glOrtho(left, right, bottom, top, zNear, zFar);
                break;
            }
            case SGL_CMD_ROTATED: {
                float angle = *((float*)pb++);
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                glRotated(angle, x, y, z);
                break;
            }
            case SGL_CMD_SCALED: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                glScaled(x, y, z);
                break;
            }
            case SGL_CMD_SCALEF: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                glScalef(x, y, z);
                break;
            }
            case SGL_CMD_POLYGONOFFSET: {
                float factor = *((float*)pb++);
                float units = *((float*)pb++);
                glPolygonOffset(factor, units);
                break;
            }
            case SGL_CMD_COPYTEXIMAGE1D: {
                int target = *pb++;
                int level = *pb++;
                int internalformat = *pb++;
                int x = *pb++;
                int y = *pb++;
                int width = *pb++;
                int border = *pb++;
                glCopyTexImage1D(target, level, internalformat, x, y, width, border);
                break;
            }
            case SGL_CMD_COPYTEXIMAGE2D: {
                int target = *pb++;
                int level = *pb++;
                int internalformat = *pb++;
                int x = *pb++;
                int y = *pb++;
                int width = *pb++;
                int height = *pb++;
                int border = *pb++;
                glCopyTexImage2D(target, level, internalformat, x, y, width, height, border);
                break;
            }
            case SGL_CMD_COPYTEXSUBIMAGE1D: {
                int target = *pb++;
                int level = *pb++;
                int xoffset = *pb++;
                int x = *pb++;
                int y = *pb++;
                int width = *pb++;
                glCopyTexSubImage1D(target, level, xoffset, x, y, width);
                break;
            }
            case SGL_CMD_COPYTEXSUBIMAGE2D: {
                int target = *pb++;
                int level = *pb++;
                int xoffset = *pb++;
                int yoffset = *pb++;
                int x = *pb++;
                int y = *pb++;
                int width = *pb++;
                int height = *pb++;
                glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
                break;
            }
            case SGL_CMD_BINDTEXTURE: {
                int target = *pb++;
                int texture = *pb++;
                glBindTexture(target, texture);
                break;
            }
            case SGL_CMD_ISTEXTURE: {
                int texture = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glIsTexture(texture);
                break;
            }
            case SGL_CMD_ARRAYELEMENT: {
                int i = *pb++;
                glArrayElement(i);
                break;
            }
            case SGL_CMD_DISABLECLIENTSTATE: {
                int array = *pb++;
                glDisableClientState(array);
                break;
            }
            case SGL_CMD_ENABLECLIENTSTATE: {
                int array = *pb++;
                glEnableClientState(array);
                break;
            }
            case SGL_CMD_INDEXUB: {
                int c = *pb++;
                glIndexub(c);
                break;
            }
            case SGL_CMD_POPCLIENTATTRIB: {
                glPopClientAttrib();
                break;
            }
            case SGL_CMD_PUSHCLIENTATTRIB: {
                int mask = *pb++;
                glPushClientAttrib(mask);
                break;
            }
            case SGL_CMD_COPYTEXSUBIMAGE3D: {
                int target = *pb++;
                int level = *pb++;
                int xoffset = *pb++;
                int yoffset = *pb++;
                int zoffset = *pb++;
                int x = *pb++;
                int y = *pb++;
                int width = *pb++;
                int height = *pb++;
                glCopyTexSubImage3D(target, level, xoffset, yoffset, zoffset, x, y, width, height);
                break;
            }
            case SGL_CMD_ACTIVETEXTURE: {
                int texture = *pb++;
                glActiveTexture(texture);
                break;
            }
            case SGL_CMD_SAMPLECOVERAGE: {
                float value = *((float*)pb++);
                int invert = *pb++;
                glSampleCoverage(value, invert);
                break;
            }
            case SGL_CMD_CLIENTACTIVETEXTURE: {
                int texture = *pb++;
                glClientActiveTexture(texture);
                break;
            }
            case SGL_CMD_MULTITEXCOORD1D: {
                int target = *pb++;
                float s = *((float*)pb++);
                glMultiTexCoord1d(target, s);
                break;
            }
            case SGL_CMD_MULTITEXCOORD1F: {
                int target = *pb++;
                float s = *((float*)pb++);
                glMultiTexCoord1f(target, s);
                break;
            }
            case SGL_CMD_MULTITEXCOORD1I: {
                int target = *pb++;
                int s = *pb++;
                glMultiTexCoord1i(target, s);
                break;
            }
            case SGL_CMD_MULTITEXCOORD1S: {
                int target = *pb++;
                int s = *pb++;
                glMultiTexCoord1s(target, s);
                break;
            }
            case SGL_CMD_MULTITEXCOORD2D: {
                int target = *pb++;
                float s = *((float*)pb++);
                float t = *((float*)pb++);
                glMultiTexCoord2d(target, s, t);
                break;
            }
            case SGL_CMD_MULTITEXCOORD2F: {
                int target = *pb++;
                float s = *((float*)pb++);
                float t = *((float*)pb++);
                glMultiTexCoord2f(target, s, t);
                break;
            }
            case SGL_CMD_MULTITEXCOORD2I: {
                int target = *pb++;
                int s = *pb++;
                int t = *pb++;
                glMultiTexCoord2i(target, s, t);
                break;
            }
            case SGL_CMD_MULTITEXCOORD2S: {
                int target = *pb++;
                int s = *pb++;
                int t = *pb++;
                glMultiTexCoord2s(target, s, t);
                break;
            }
            case SGL_CMD_MULTITEXCOORD3D: {
                int target = *pb++;
                float s = *((float*)pb++);
                float t = *((float*)pb++);
                float r = *((float*)pb++);
                glMultiTexCoord3d(target, s, t, r);
                break;
            }
            case SGL_CMD_MULTITEXCOORD3F: {
                int target = *pb++;
                float s = *((float*)pb++);
                float t = *((float*)pb++);
                float r = *((float*)pb++);
                glMultiTexCoord3f(target, s, t, r);
                break;
            }
            case SGL_CMD_MULTITEXCOORD3I: {
                int target = *pb++;
                int s = *pb++;
                int t = *pb++;
                int r = *pb++;
                glMultiTexCoord3i(target, s, t, r);
                break;
            }
            case SGL_CMD_MULTITEXCOORD3S: {
                int target = *pb++;
                int s = *pb++;
                int t = *pb++;
                int r = *pb++;
                glMultiTexCoord3s(target, s, t, r);
                break;
            }
            case SGL_CMD_MULTITEXCOORD4D: {
                int target = *pb++;
                float s = *((float*)pb++);
                float t = *((float*)pb++);
                float r = *((float*)pb++);
                float q = *((float*)pb++);
                glMultiTexCoord4d(target, s, t, r, q);
                break;
            }
            case SGL_CMD_MULTITEXCOORD4F: {
                int target = *pb++;
                float s = *((float*)pb++);
                float t = *((float*)pb++);
                float r = *((float*)pb++);
                float q = *((float*)pb++);
                glMultiTexCoord4f(target, s, t, r, q);
                break;
            }
            case SGL_CMD_MULTITEXCOORD4I: {
                int target = *pb++;
                int s = *pb++;
                int t = *pb++;
                int r = *pb++;
                int q = *pb++;
                glMultiTexCoord4i(target, s, t, r, q);
                break;
            }
            case SGL_CMD_MULTITEXCOORD4S: {
                int target = *pb++;
                int s = *pb++;
                int t = *pb++;
                int r = *pb++;
                int q = *pb++;
                glMultiTexCoord4s(target, s, t, r, q);
                break;
            }
            case SGL_CMD_BLENDFUNCSEPARATE: {
                int sfactorRGB = *pb++;
                int dfactorRGB = *pb++;
                int sfactorAlpha = *pb++;
                int dfactorAlpha = *pb++;
                glBlendFuncSeparate(sfactorRGB, dfactorRGB, sfactorAlpha, dfactorAlpha);
                break;
            }
            case SGL_CMD_POINTPARAMETERF: {
                int pname = *pb++;
                float param = *((float*)pb++);
                glPointParameterf(pname, param);
                break;
            }
            case SGL_CMD_POINTPARAMETERI: {
                int pname = *pb++;
                int param = *pb++;
                glPointParameteri(pname, param);
                break;
            }
            case SGL_CMD_FOGCOORDF: {
                float coord = *((float*)pb++);
                glFogCoordf(coord);
                break;
            }
            case SGL_CMD_FOGCOORDD: {
                float coord = *((float*)pb++);
                glFogCoordd(coord);
                break;
            }
            case SGL_CMD_SECONDARYCOLOR3B: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                glSecondaryColor3b(red, green, blue);
                break;
            }
            case SGL_CMD_SECONDARYCOLOR3D: {
                float red = *((float*)pb++);
                float green = *((float*)pb++);
                float blue = *((float*)pb++);
                glSecondaryColor3d(red, green, blue);
                break;
            }
            case SGL_CMD_SECONDARYCOLOR3F: {
                float red = *((float*)pb++);
                float green = *((float*)pb++);
                float blue = *((float*)pb++);
                glSecondaryColor3f(red, green, blue);
                break;
            }
            case SGL_CMD_SECONDARYCOLOR3I: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                glSecondaryColor3i(red, green, blue);
                break;
            }
            case SGL_CMD_SECONDARYCOLOR3S: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                glSecondaryColor3s(red, green, blue);
                break;
            }
            case SGL_CMD_SECONDARYCOLOR3UB: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                glSecondaryColor3ub(red, green, blue);
                break;
            }
            case SGL_CMD_SECONDARYCOLOR3UI: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                glSecondaryColor3ui(red, green, blue);
                break;
            }
            case SGL_CMD_SECONDARYCOLOR3US: {
                int red = *pb++;
                int green = *pb++;
                int blue = *pb++;
                glSecondaryColor3us(red, green, blue);
                break;
            }
            case SGL_CMD_WINDOWPOS2D: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                glWindowPos2d(x, y);
                break;
            }
            case SGL_CMD_WINDOWPOS2F: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                glWindowPos2f(x, y);
                break;
            }
            case SGL_CMD_WINDOWPOS2I: {
                int x = *pb++;
                int y = *pb++;
                glWindowPos2i(x, y);
                break;
            }
            case SGL_CMD_WINDOWPOS2S: {
                int x = *pb++;
                int y = *pb++;
                glWindowPos2s(x, y);
                break;
            }
            case SGL_CMD_WINDOWPOS3D: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                glWindowPos3d(x, y, z);
                break;
            }
            case SGL_CMD_WINDOWPOS3F: {
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                glWindowPos3f(x, y, z);
                break;
            }
            case SGL_CMD_WINDOWPOS3I: {
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                glWindowPos3i(x, y, z);
                break;
            }
            case SGL_CMD_WINDOWPOS3S: {
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                glWindowPos3s(x, y, z);
                break;
            }
            case SGL_CMD_BLENDCOLOR: {
                float red = *((float*)pb++);
                float green = *((float*)pb++);
                float blue = *((float*)pb++);
                float alpha = *((float*)pb++);
                glBlendColor(red, green, blue, alpha);
                break;
            }
            case SGL_CMD_BLENDEQUATION: {
                int mode = *pb++;
                glBlendEquation(mode);
                break;
            }
            case SGL_CMD_ISQUERY: {
                int id = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glIsQuery(id);
                break;
            }
            case SGL_CMD_ISBUFFER: {
                int buffer = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glIsBuffer(buffer);
                break;
            }
            case SGL_CMD_UNMAPBUFFER: {
                int target = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glUnmapBuffer(target);
                break;
            }
            case SGL_CMD_BLENDEQUATIONSEPARATE: {
                int modeRGB = *pb++;
                int modeAlpha = *pb++;
                glBlendEquationSeparate(modeRGB, modeAlpha);
                break;
            }
            case SGL_CMD_STENCILOPSEPARATE: {
                int face = *pb++;
                int sfail = *pb++;
                int dpfail = *pb++;
                int dppass = *pb++;
                glStencilOpSeparate(face, sfail, dpfail, dppass);
                break;
            }
            case SGL_CMD_STENCILFUNCSEPARATE: {
                int face = *pb++;
                int func = *pb++;
                int ref = *pb++;
                int mask = *pb++;
                glStencilFuncSeparate(face, func, ref, mask);
                break;
            }
            case SGL_CMD_STENCILMASKSEPARATE: {
                int face = *pb++;
                int mask = *pb++;
                glStencilMaskSeparate(face, mask);
                break;
            }
            case SGL_CMD_DISABLEVERTEXATTRIBARRAY: {
                int index = *pb++;
                glDisableVertexAttribArray(index);
                break;
            }
            case SGL_CMD_ISPROGRAM: {
                int program = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glIsProgram(program);
                break;
            }
            case SGL_CMD_ISSHADER: {
                int shader = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glIsShader(shader);
                break;
            }
            case SGL_CMD_UNIFORM2F: {
                int location = *pb++;
                float v0 = *((float*)pb++);
                float v1 = *((float*)pb++);
                glUniform2f(location, v0, v1);
                break;
            }
            case SGL_CMD_UNIFORM3F: {
                int location = *pb++;
                float v0 = *((float*)pb++);
                float v1 = *((float*)pb++);
                float v2 = *((float*)pb++);
                glUniform3f(location, v0, v1, v2);
                break;
            }
            case SGL_CMD_UNIFORM4F: {
                int location = *pb++;
                float v0 = *((float*)pb++);
                float v1 = *((float*)pb++);
                float v2 = *((float*)pb++);
                float v3 = *((float*)pb++);
                glUniform4f(location, v0, v1, v2, v3);
                break;
            }
            case SGL_CMD_UNIFORM1I: {
                int location = *pb++;
                int v0 = *pb++;
                glUniform1i(location, v0);
                break;
            }
            case SGL_CMD_UNIFORM2I: {
                int location = *pb++;
                int v0 = *pb++;
                int v1 = *pb++;
                glUniform2i(location, v0, v1);
                break;
            }
            case SGL_CMD_UNIFORM3I: {
                int location = *pb++;
                int v0 = *pb++;
                int v1 = *pb++;
                int v2 = *pb++;
                glUniform3i(location, v0, v1, v2);
                break;
            }
            case SGL_CMD_UNIFORM4I: {
                int location = *pb++;
                int v0 = *pb++;
                int v1 = *pb++;
                int v2 = *pb++;
                int v3 = *pb++;
                glUniform4i(location, v0, v1, v2, v3);
                break;
            }
            case SGL_CMD_VALIDATEPROGRAM: {
                int program = *pb++;
                glValidateProgram(program);
                break;
            }
            case SGL_CMD_VERTEXATTRIB1D: {
                int index = *pb++;
                float x = *((float*)pb++);
                glVertexAttrib1d(index, x);
                break;
            }
            case SGL_CMD_VERTEXATTRIB1F: {
                int index = *pb++;
                float x = *((float*)pb++);
                glVertexAttrib1f(index, x);
                break;
            }
            case SGL_CMD_VERTEXATTRIB1S: {
                int index = *pb++;
                int x = *pb++;
                glVertexAttrib1s(index, x);
                break;
            }
            case SGL_CMD_VERTEXATTRIB2D: {
                int index = *pb++;
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                glVertexAttrib2d(index, x, y);
                break;
            }
            case SGL_CMD_VERTEXATTRIB2F: {
                int index = *pb++;
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                glVertexAttrib2f(index, x, y);
                break;
            }
            case SGL_CMD_VERTEXATTRIB2S: {
                int index = *pb++;
                int x = *pb++;
                int y = *pb++;
                glVertexAttrib2s(index, x, y);
                break;
            }
            case SGL_CMD_VERTEXATTRIB3D: {
                int index = *pb++;
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                glVertexAttrib3d(index, x, y, z);
                break;
            }
            case SGL_CMD_VERTEXATTRIB3F: {
                int index = *pb++;
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                glVertexAttrib3f(index, x, y, z);
                break;
            }
            case SGL_CMD_VERTEXATTRIB3S: {
                int index = *pb++;
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                glVertexAttrib3s(index, x, y, z);
                break;
            }
            case SGL_CMD_VERTEXATTRIB4NUB: {
                int index = *pb++;
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                int w = *pb++;
                glVertexAttrib4Nub(index, x, y, z, w);
                break;
            }
            case SGL_CMD_VERTEXATTRIB4D: {
                int index = *pb++;
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                float w = *((float*)pb++);
                glVertexAttrib4d(index, x, y, z, w);
                break;
            }
            case SGL_CMD_VERTEXATTRIB4F: {
                int index = *pb++;
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                float w = *((float*)pb++);
                glVertexAttrib4f(index, x, y, z, w);
                break;
            }
            case SGL_CMD_VERTEXATTRIB4S: {
                int index = *pb++;
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                int w = *pb++;
                glVertexAttrib4s(index, x, y, z, w);
                break;
            }
            case SGL_CMD_COLORMASKI: {
                int index = *pb++;
                int r = *pb++;
                int g = *pb++;
                int b = *pb++;
                int a = *pb++;
                glColorMaski(index, r, g, b, a);
                break;
            }
            case SGL_CMD_ENABLEI: {
                int target = *pb++;
                int index = *pb++;
                glEnablei(target, index);
                break;
            }
            case SGL_CMD_DISABLEI: {
                int target = *pb++;
                int index = *pb++;
                glDisablei(target, index);
                break;
            }
            case SGL_CMD_ISENABLEDI: {
                int target = *pb++;
                int index = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glIsEnabledi(target, index);
                break;
            }
            case SGL_CMD_BEGINTRANSFORMFEEDBACK: {
                int primitiveMode = *pb++;
                glBeginTransformFeedback(primitiveMode);
                break;
            }
            case SGL_CMD_ENDTRANSFORMFEEDBACK: {
                glEndTransformFeedback();
                break;
            }
            case SGL_CMD_BINDBUFFERRANGE: {
                int target = *pb++;
                int index = *pb++;
                int buffer = *pb++;
                int offset = *pb++;
                int size = *pb++;
                glBindBufferRange(target, index, buffer, offset, size);
                break;
            }
            case SGL_CMD_BINDBUFFERBASE: {
                int target = *pb++;
                int index = *pb++;
                int buffer = *pb++;
                glBindBufferBase(target, index, buffer);
                break;
            }
            case SGL_CMD_CLAMPCOLOR: {
                int target = *pb++;
                int clamp = *pb++;
                glClampColor(target, clamp);
                break;
            }
            case SGL_CMD_BEGINCONDITIONALRENDER: {
                int id = *pb++;
                int mode = *pb++;
                glBeginConditionalRender(id, mode);
                break;
            }
            case SGL_CMD_ENDCONDITIONALRENDER: {
                glEndConditionalRender();
                break;
            }
            case SGL_CMD_VERTEXATTRIBI1I: {
                int index = *pb++;
                int x = *pb++;
                glVertexAttribI1i(index, x);
                break;
            }
            case SGL_CMD_VERTEXATTRIBI2I: {
                int index = *pb++;
                int x = *pb++;
                int y = *pb++;
                glVertexAttribI2i(index, x, y);
                break;
            }
            case SGL_CMD_VERTEXATTRIBI3I: {
                int index = *pb++;
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                glVertexAttribI3i(index, x, y, z);
                break;
            }
            case SGL_CMD_VERTEXATTRIBI4I: {
                int index = *pb++;
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                int w = *pb++;
                glVertexAttribI4i(index, x, y, z, w);
                break;
            }
            case SGL_CMD_VERTEXATTRIBI1UI: {
                int index = *pb++;
                int x = *pb++;
                glVertexAttribI1ui(index, x);
                break;
            }
            case SGL_CMD_VERTEXATTRIBI2UI: {
                int index = *pb++;
                int x = *pb++;
                int y = *pb++;
                glVertexAttribI2ui(index, x, y);
                break;
            }
            case SGL_CMD_VERTEXATTRIBI3UI: {
                int index = *pb++;
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                glVertexAttribI3ui(index, x, y, z);
                break;
            }
            case SGL_CMD_VERTEXATTRIBI4UI: {
                int index = *pb++;
                int x = *pb++;
                int y = *pb++;
                int z = *pb++;
                int w = *pb++;
                glVertexAttribI4ui(index, x, y, z, w);
                break;
            }
            case SGL_CMD_UNIFORM1UI: {
                int location = *pb++;
                int v0 = *pb++;
                glUniform1ui(location, v0);
                break;
            }
            case SGL_CMD_UNIFORM2UI: {
                int location = *pb++;
                int v0 = *pb++;
                int v1 = *pb++;
                glUniform2ui(location, v0, v1);
                break;
            }
            case SGL_CMD_UNIFORM3UI: {
                int location = *pb++;
                int v0 = *pb++;
                int v1 = *pb++;
                int v2 = *pb++;
                glUniform3ui(location, v0, v1, v2);
                break;
            }
            case SGL_CMD_UNIFORM4UI: {
                int location = *pb++;
                int v0 = *pb++;
                int v1 = *pb++;
                int v2 = *pb++;
                int v3 = *pb++;
                glUniform4ui(location, v0, v1, v2, v3);
                break;
            }
            case SGL_CMD_CLEARBUFFERFI: {
                int buffer = *pb++;
                int drawbuffer = *pb++;
                float depth = *((float*)pb++);
                int stencil = *pb++;
                glClearBufferfi(buffer, drawbuffer, depth, stencil);
                break;
            }
            case SGL_CMD_ISRENDERBUFFER: {
                int renderbuffer = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glIsRenderbuffer(renderbuffer);
                break;
            }
            case SGL_CMD_BINDRENDERBUFFER: {
                int target = *pb++;
                int renderbuffer = *pb++;
                glBindRenderbuffer(target, renderbuffer);
                break;
            }
            case SGL_CMD_RENDERBUFFERSTORAGE: {
                int target = *pb++;
                int internalformat = *pb++;
                int width = *pb++;
                int height = *pb++;
                glRenderbufferStorage(target, internalformat, width, height);
                break;
            }
            case SGL_CMD_ISFRAMEBUFFER: {
                int framebuffer = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glIsFramebuffer(framebuffer);
                break;
            }
            case SGL_CMD_BINDFRAMEBUFFER: {
                int target = *pb++;
                int framebuffer = *pb++;
                glBindFramebuffer(target, framebuffer);
                break;
            }
            case SGL_CMD_CHECKFRAMEBUFFERSTATUS: {
                int target = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glCheckFramebufferStatus(target);
                break;
            }
            case SGL_CMD_FRAMEBUFFERTEXTURE1D: {
                int target = *pb++;
                int attachment = *pb++;
                int textarget = *pb++;
                int texture = *pb++;
                int level = *pb++;
                glFramebufferTexture1D(target, attachment, textarget, texture, level);
                break;
            }
            case SGL_CMD_FRAMEBUFFERTEXTURE2D: {
                int target = *pb++;
                int attachment = *pb++;
                int textarget = *pb++;
                int texture = *pb++;
                int level = *pb++;
                glFramebufferTexture2D(target, attachment, textarget, texture, level);
                break;
            }
            case SGL_CMD_FRAMEBUFFERTEXTURE3D: {
                int target = *pb++;
                int attachment = *pb++;
                int textarget = *pb++;
                int texture = *pb++;
                int level = *pb++;
                int zoffset = *pb++;
                glFramebufferTexture3D(target, attachment, textarget, texture, level, zoffset);
                break;
            }
            case SGL_CMD_FRAMEBUFFERRENDERBUFFER: {
                int target = *pb++;
                int attachment = *pb++;
                int renderbuffertarget = *pb++;
                int renderbuffer = *pb++;
                glFramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer);
                break;
            }
            case SGL_CMD_GENERATEMIPMAP: {
                int target = *pb++;
                glGenerateMipmap(target);
                break;
            }
            case SGL_CMD_BLITFRAMEBUFFER: {
                int srcX0 = *pb++;
                int srcY0 = *pb++;
                int srcX1 = *pb++;
                int srcY1 = *pb++;
                int dstX0 = *pb++;
                int dstY0 = *pb++;
                int dstX1 = *pb++;
                int dstY1 = *pb++;
                int mask = *pb++;
                int filter = *pb++;
                glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
                break;
            }
            case SGL_CMD_RENDERBUFFERSTORAGEMULTISAMPLE: {
                int target = *pb++;
                int samples = *pb++;
                int internalformat = *pb++;
                int width = *pb++;
                int height = *pb++;
                glRenderbufferStorageMultisample(target, samples, internalformat, width, height);
                break;
            }
            case SGL_CMD_FRAMEBUFFERTEXTURELAYER: {
                int target = *pb++;
                int attachment = *pb++;
                int texture = *pb++;
                int level = *pb++;
                int layer = *pb++;
                glFramebufferTextureLayer(target, attachment, texture, level, layer);
                break;
            }
            case SGL_CMD_FLUSHMAPPEDBUFFERRANGE: {
                int target = *pb++;
                int offset = *pb++;
                int length = *pb++;
                glFlushMappedBufferRange(target, offset, length);
                break;
            }
            case SGL_CMD_ISVERTEXARRAY: {
                int array = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glIsVertexArray(array);
                break;
            }
            case SGL_CMD_DRAWARRAYSINSTANCED: {
                int mode = *pb++;
                int first = *pb++;
                int count = *pb++;
                int instancecount = *pb++;
                glDrawArraysInstanced(mode, first, count, instancecount);
                break;
            }
            case SGL_CMD_TEXBUFFER: {
                int target = *pb++;
                int internalformat = *pb++;
                int buffer = *pb++;
                glTexBuffer(target, internalformat, buffer);
                break;
            }
            case SGL_CMD_PRIMITIVERESTARTINDEX: {
                int index = *pb++;
                glPrimitiveRestartIndex(index);
                break;
            }
            case SGL_CMD_COPYBUFFERSUBDATA: {
                int readTarget = *pb++;
                int writeTarget = *pb++;
                int readOffset = *pb++;
                int writeOffset = *pb++;
                int size = *pb++;
                glCopyBufferSubData(readTarget, writeTarget, readOffset, writeOffset, size);
                break;
            }
            case SGL_CMD_UNIFORMBLOCKBINDING: {
                int program = *pb++;
                int uniformBlockIndex = *pb++;
                int uniformBlockBinding = *pb++;
                glUniformBlockBinding(program, uniformBlockIndex, uniformBlockBinding);
                break;
            }
            case SGL_CMD_PROVOKINGVERTEX: {
                int mode = *pb++;
                glProvokingVertex(mode);
                break;
            }
            case SGL_CMD_FRAMEBUFFERTEXTURE: {
                int target = *pb++;
                int attachment = *pb++;
                int texture = *pb++;
                int level = *pb++;
                glFramebufferTexture(target, attachment, texture, level);
                break;
            }
            case SGL_CMD_TEXIMAGE2DMULTISAMPLE: {
                int target = *pb++;
                int samples = *pb++;
                int internalformat = *pb++;
                int width = *pb++;
                int height = *pb++;
                int fixedsamplelocations = *pb++;
                glTexImage2DMultisample(target, samples, internalformat, width, height, fixedsamplelocations);
                break;
            }
            case SGL_CMD_TEXIMAGE3DMULTISAMPLE: {
                int target = *pb++;
                int samples = *pb++;
                int internalformat = *pb++;
                int width = *pb++;
                int height = *pb++;
                int depth = *pb++;
                int fixedsamplelocations = *pb++;
                glTexImage3DMultisample(target, samples, internalformat, width, height, depth, fixedsamplelocations);
                break;
            }
            case SGL_CMD_SAMPLEMASKI: {
                int maskNumber = *pb++;
                int mask = *pb++;
                glSampleMaski(maskNumber, mask);
                break;
            }
            case SGL_CMD_ISSAMPLER: {
                int sampler = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glIsSampler(sampler);
                break;
            }
            case SGL_CMD_BINDSAMPLER: {
                int unit = *pb++;
                int sampler = *pb++;
                glBindSampler(unit, sampler);
                break;
            }
            case SGL_CMD_SAMPLERPARAMETERI: {
                int sampler = *pb++;
                int pname = *pb++;
                int param = *pb++;
                glSamplerParameteri(sampler, pname, param);
                break;
            }
            case SGL_CMD_SAMPLERPARAMETERF: {
                int sampler = *pb++;
                int pname = *pb++;
                float param = *((float*)pb++);
                glSamplerParameterf(sampler, pname, param);
                break;
            }
            case SGL_CMD_QUERYCOUNTER: {
                int id = *pb++;
                int target = *pb++;
                glQueryCounter(id, target);
                break;
            }
            case SGL_CMD_VERTEXATTRIBDIVISOR: {
                int index = *pb++;
                int divisor = *pb++;
                glVertexAttribDivisor(index, divisor);
                break;
            }
            case SGL_CMD_VERTEXATTRIBP1UI: {
                int index = *pb++;
                int type = *pb++;
                int normalized = *pb++;
                int value = *pb++;
                glVertexAttribP1ui(index, type, normalized, value);
                break;
            }
            case SGL_CMD_VERTEXATTRIBP2UI: {
                int index = *pb++;
                int type = *pb++;
                int normalized = *pb++;
                int value = *pb++;
                glVertexAttribP2ui(index, type, normalized, value);
                break;
            }
            case SGL_CMD_VERTEXATTRIBP3UI: {
                int index = *pb++;
                int type = *pb++;
                int normalized = *pb++;
                int value = *pb++;
                glVertexAttribP3ui(index, type, normalized, value);
                break;
            }
            case SGL_CMD_VERTEXATTRIBP4UI: {
                int index = *pb++;
                int type = *pb++;
                int normalized = *pb++;
                int value = *pb++;
                glVertexAttribP4ui(index, type, normalized, value);
                break;
            }
            case SGL_CMD_VERTEXP2UI: {
                int type = *pb++;
                int value = *pb++;
                glVertexP2ui(type, value);
                break;
            }
            case SGL_CMD_VERTEXP3UI: {
                int type = *pb++;
                int value = *pb++;
                glVertexP3ui(type, value);
                break;
            }
            case SGL_CMD_VERTEXP4UI: {
                int type = *pb++;
                int value = *pb++;
                glVertexP4ui(type, value);
                break;
            }
            case SGL_CMD_TEXCOORDP1UI: {
                int type = *pb++;
                int coords = *pb++;
                glTexCoordP1ui(type, coords);
                break;
            }
            case SGL_CMD_TEXCOORDP2UI: {
                int type = *pb++;
                int coords = *pb++;
                glTexCoordP2ui(type, coords);
                break;
            }
            case SGL_CMD_TEXCOORDP3UI: {
                int type = *pb++;
                int coords = *pb++;
                glTexCoordP3ui(type, coords);
                break;
            }
            case SGL_CMD_TEXCOORDP4UI: {
                int type = *pb++;
                int coords = *pb++;
                glTexCoordP4ui(type, coords);
                break;
            }
            case SGL_CMD_MULTITEXCOORDP1UI: {
                int texture = *pb++;
                int type = *pb++;
                int coords = *pb++;
                glMultiTexCoordP1ui(texture, type, coords);
                break;
            }
            case SGL_CMD_MULTITEXCOORDP2UI: {
                int texture = *pb++;
                int type = *pb++;
                int coords = *pb++;
                glMultiTexCoordP2ui(texture, type, coords);
                break;
            }
            case SGL_CMD_MULTITEXCOORDP3UI: {
                int texture = *pb++;
                int type = *pb++;
                int coords = *pb++;
                glMultiTexCoordP3ui(texture, type, coords);
                break;
            }
            case SGL_CMD_MULTITEXCOORDP4UI: {
                int texture = *pb++;
                int type = *pb++;
                int coords = *pb++;
                glMultiTexCoordP4ui(texture, type, coords);
                break;
            }
            case SGL_CMD_NORMALP3UI: {
                int type = *pb++;
                int coords = *pb++;
                glNormalP3ui(type, coords);
                break;
            }
            case SGL_CMD_COLORP3UI: {
                int type = *pb++;
                int color = *pb++;
                glColorP3ui(type, color);
                break;
            }
            case SGL_CMD_COLORP4UI: {
                int type = *pb++;
                int color = *pb++;
                glColorP4ui(type, color);
                break;
            }
            case SGL_CMD_SECONDARYCOLORP3UI: {
                int type = *pb++;
                int color = *pb++;
                glSecondaryColorP3ui(type, color);
                break;
            }
            case SGL_CMD_MINSAMPLESHADING: {
                float value = *((float*)pb++);
                glMinSampleShading(value);
                break;
            }
            case SGL_CMD_BLENDEQUATIONI: {
                int buf = *pb++;
                int mode = *pb++;
                glBlendEquationi(buf, mode);
                break;
            }
            case SGL_CMD_BLENDEQUATIONSEPARATEI: {
                int buf = *pb++;
                int modeRGB = *pb++;
                int modeAlpha = *pb++;
                glBlendEquationSeparatei(buf, modeRGB, modeAlpha);
                break;
            }
            case SGL_CMD_BLENDFUNCI: {
                int buf = *pb++;
                int src = *pb++;
                int dst = *pb++;
                glBlendFunci(buf, src, dst);
                break;
            }
            case SGL_CMD_BLENDFUNCSEPARATEI: {
                int buf = *pb++;
                int srcRGB = *pb++;
                int dstRGB = *pb++;
                int srcAlpha = *pb++;
                int dstAlpha = *pb++;
                glBlendFuncSeparatei(buf, srcRGB, dstRGB, srcAlpha, dstAlpha);
                break;
            }
            case SGL_CMD_UNIFORM1D: {
                int location = *pb++;
                float x = *((float*)pb++);
                glUniform1d(location, x);
                break;
            }
            case SGL_CMD_UNIFORM2D: {
                int location = *pb++;
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                glUniform2d(location, x, y);
                break;
            }
            case SGL_CMD_UNIFORM3D: {
                int location = *pb++;
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                glUniform3d(location, x, y, z);
                break;
            }
            case SGL_CMD_UNIFORM4D: {
                int location = *pb++;
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                float w = *((float*)pb++);
                glUniform4d(location, x, y, z, w);
                break;
            }
            case SGL_CMD_PATCHPARAMETERI: {
                int pname = *pb++;
                int value = *pb++;
                glPatchParameteri(pname, value);
                break;
            }
            case SGL_CMD_BINDTRANSFORMFEEDBACK: {
                int target = *pb++;
                int id = *pb++;
                glBindTransformFeedback(target, id);
                break;
            }
            case SGL_CMD_ISTRANSFORMFEEDBACK: {
                int id = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glIsTransformFeedback(id);
                break;
            }
            case SGL_CMD_PAUSETRANSFORMFEEDBACK: {
                glPauseTransformFeedback();
                break;
            }
            case SGL_CMD_RESUMETRANSFORMFEEDBACK: {
                glResumeTransformFeedback();
                break;
            }
            case SGL_CMD_DRAWTRANSFORMFEEDBACK: {
                int mode = *pb++;
                int id = *pb++;
                glDrawTransformFeedback(mode, id);
                break;
            }
            case SGL_CMD_DRAWTRANSFORMFEEDBACKSTREAM: {
                int mode = *pb++;
                int id = *pb++;
                int stream = *pb++;
                glDrawTransformFeedbackStream(mode, id, stream);
                break;
            }
            case SGL_CMD_BEGINQUERYINDEXED: {
                int target = *pb++;
                int index = *pb++;
                int id = *pb++;
                glBeginQueryIndexed(target, index, id);
                break;
            }
            case SGL_CMD_ENDQUERYINDEXED: {
                int target = *pb++;
                int index = *pb++;
                glEndQueryIndexed(target, index);
                break;
            }
            case SGL_CMD_RELEASESHADERCOMPILER: {
                glReleaseShaderCompiler();
                break;
            }
            case SGL_CMD_DEPTHRANGEF: {
                float n = *((float*)pb++);
                float f = *((float*)pb++);
                glDepthRangef(n, f);
                break;
            }
            case SGL_CMD_CLEARDEPTHF: {
                float d = *((float*)pb++);
                glClearDepthf(d);
                break;
            }
            case SGL_CMD_PROGRAMPARAMETERI: {
                int program = *pb++;
                int pname = *pb++;
                int value = *pb++;
                glProgramParameteri(program, pname, value);
                break;
            }
            case SGL_CMD_USEPROGRAMSTAGES: {
                int pipeline = *pb++;
                int stages = *pb++;
                int program = *pb++;
                glUseProgramStages(pipeline, stages, program);
                break;
            }
            case SGL_CMD_ACTIVESHADERPROGRAM: {
                int pipeline = *pb++;
                int program = *pb++;
                glActiveShaderProgram(pipeline, program);
                break;
            }
            case SGL_CMD_BINDPROGRAMPIPELINE: {
                int pipeline = *pb++;
                glBindProgramPipeline(pipeline);
                break;
            }
            case SGL_CMD_ISPROGRAMPIPELINE: {
                int pipeline = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glIsProgramPipeline(pipeline);
                break;
            }
            case SGL_CMD_PROGRAMUNIFORM1I: {
                int program = *pb++;
                int location = *pb++;
                int v0 = *pb++;
                glProgramUniform1i(program, location, v0);
                break;
            }
            case SGL_CMD_PROGRAMUNIFORM1F: {
                int program = *pb++;
                int location = *pb++;
                float v0 = *((float*)pb++);
                glProgramUniform1f(program, location, v0);
                break;
            }
            case SGL_CMD_PROGRAMUNIFORM1D: {
                int program = *pb++;
                int location = *pb++;
                float v0 = *((float*)pb++);
                glProgramUniform1d(program, location, v0);
                break;
            }
            case SGL_CMD_PROGRAMUNIFORM1UI: {
                int program = *pb++;
                int location = *pb++;
                int v0 = *pb++;
                glProgramUniform1ui(program, location, v0);
                break;
            }
            case SGL_CMD_PROGRAMUNIFORM2I: {
                int program = *pb++;
                int location = *pb++;
                int v0 = *pb++;
                int v1 = *pb++;
                glProgramUniform2i(program, location, v0, v1);
                break;
            }
            case SGL_CMD_PROGRAMUNIFORM2F: {
                int program = *pb++;
                int location = *pb++;
                float v0 = *((float*)pb++);
                float v1 = *((float*)pb++);
                glProgramUniform2f(program, location, v0, v1);
                break;
            }
            case SGL_CMD_PROGRAMUNIFORM2D: {
                int program = *pb++;
                int location = *pb++;
                float v0 = *((float*)pb++);
                float v1 = *((float*)pb++);
                glProgramUniform2d(program, location, v0, v1);
                break;
            }
            case SGL_CMD_PROGRAMUNIFORM2UI: {
                int program = *pb++;
                int location = *pb++;
                int v0 = *pb++;
                int v1 = *pb++;
                glProgramUniform2ui(program, location, v0, v1);
                break;
            }
            case SGL_CMD_PROGRAMUNIFORM3I: {
                int program = *pb++;
                int location = *pb++;
                int v0 = *pb++;
                int v1 = *pb++;
                int v2 = *pb++;
                glProgramUniform3i(program, location, v0, v1, v2);
                break;
            }
            case SGL_CMD_PROGRAMUNIFORM3F: {
                int program = *pb++;
                int location = *pb++;
                float v0 = *((float*)pb++);
                float v1 = *((float*)pb++);
                float v2 = *((float*)pb++);
                glProgramUniform3f(program, location, v0, v1, v2);
                break;
            }
            case SGL_CMD_PROGRAMUNIFORM3D: {
                int program = *pb++;
                int location = *pb++;
                float v0 = *((float*)pb++);
                float v1 = *((float*)pb++);
                float v2 = *((float*)pb++);
                glProgramUniform3d(program, location, v0, v1, v2);
                break;
            }
            case SGL_CMD_PROGRAMUNIFORM3UI: {
                int program = *pb++;
                int location = *pb++;
                int v0 = *pb++;
                int v1 = *pb++;
                int v2 = *pb++;
                glProgramUniform3ui(program, location, v0, v1, v2);
                break;
            }
            case SGL_CMD_PROGRAMUNIFORM4I: {
                int program = *pb++;
                int location = *pb++;
                int v0 = *pb++;
                int v1 = *pb++;
                int v2 = *pb++;
                int v3 = *pb++;
                glProgramUniform4i(program, location, v0, v1, v2, v3);
                break;
            }
            case SGL_CMD_PROGRAMUNIFORM4F: {
                int program = *pb++;
                int location = *pb++;
                float v0 = *((float*)pb++);
                float v1 = *((float*)pb++);
                float v2 = *((float*)pb++);
                float v3 = *((float*)pb++);
                glProgramUniform4f(program, location, v0, v1, v2, v3);
                break;
            }
            case SGL_CMD_PROGRAMUNIFORM4D: {
                int program = *pb++;
                int location = *pb++;
                float v0 = *((float*)pb++);
                float v1 = *((float*)pb++);
                float v2 = *((float*)pb++);
                float v3 = *((float*)pb++);
                glProgramUniform4d(program, location, v0, v1, v2, v3);
                break;
            }
            case SGL_CMD_PROGRAMUNIFORM4UI: {
                int program = *pb++;
                int location = *pb++;
                int v0 = *pb++;
                int v1 = *pb++;
                int v2 = *pb++;
                int v3 = *pb++;
                glProgramUniform4ui(program, location, v0, v1, v2, v3);
                break;
            }
            case SGL_CMD_VALIDATEPROGRAMPIPELINE: {
                int pipeline = *pb++;
                glValidateProgramPipeline(pipeline);
                break;
            }
            case SGL_CMD_VERTEXATTRIBL1D: {
                int index = *pb++;
                float x = *((float*)pb++);
                glVertexAttribL1d(index, x);
                break;
            }
            case SGL_CMD_VERTEXATTRIBL2D: {
                int index = *pb++;
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                glVertexAttribL2d(index, x, y);
                break;
            }
            case SGL_CMD_VERTEXATTRIBL3D: {
                int index = *pb++;
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                glVertexAttribL3d(index, x, y, z);
                break;
            }
            case SGL_CMD_VERTEXATTRIBL4D: {
                int index = *pb++;
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float z = *((float*)pb++);
                float w = *((float*)pb++);
                glVertexAttribL4d(index, x, y, z, w);
                break;
            }
            case SGL_CMD_VIEWPORTINDEXEDF: {
                int index = *pb++;
                float x = *((float*)pb++);
                float y = *((float*)pb++);
                float w = *((float*)pb++);
                float h = *((float*)pb++);
                glViewportIndexedf(index, x, y, w, h);
                break;
            }
            case SGL_CMD_SCISSORINDEXED: {
                int index = *pb++;
                int left = *pb++;
                int bottom = *pb++;
                int width = *pb++;
                int height = *pb++;
                glScissorIndexed(index, left, bottom, width, height);
                break;
            }
            case SGL_CMD_DEPTHRANGEINDEXED: {
                int index = *pb++;
                float n = *((float*)pb++);
                float f = *((float*)pb++);
                glDepthRangeIndexed(index, n, f);
                break;
            }
            case SGL_CMD_DRAWARRAYSINSTANCEDBASEINSTANCE: {
                int mode = *pb++;
                int first = *pb++;
                int count = *pb++;
                int instancecount = *pb++;
                int baseinstance = *pb++;
                glDrawArraysInstancedBaseInstance(mode, first, count, instancecount, baseinstance);
                break;
            }
            case SGL_CMD_BINDIMAGETEXTURE: {
                int unit = *pb++;
                int texture = *pb++;
                int level = *pb++;
                int layered = *pb++;
                int layer = *pb++;
                int access = *pb++;
                int format = *pb++;
                glBindImageTexture(unit, texture, level, layered, layer, access, format);
                break;
            }
            case SGL_CMD_MEMORYBARRIER: {
                int barriers = *pb++;
                glMemoryBarrier(barriers);
                break;
            }
            case SGL_CMD_TEXSTORAGE1D: {
                int target = *pb++;
                int levels = *pb++;
                int internalformat = *pb++;
                int width = *pb++;
                glTexStorage1D(target, levels, internalformat, width);
                break;
            }
            case SGL_CMD_TEXSTORAGE2D: {
                int target = *pb++;
                int levels = *pb++;
                int internalformat = *pb++;
                int width = *pb++;
                int height = *pb++;
                glTexStorage2D(target, levels, internalformat, width, height);
                break;
            }
            case SGL_CMD_TEXSTORAGE3D: {
                int target = *pb++;
                int levels = *pb++;
                int internalformat = *pb++;
                int width = *pb++;
                int height = *pb++;
                int depth = *pb++;
                glTexStorage3D(target, levels, internalformat, width, height, depth);
                break;
            }
            case SGL_CMD_DRAWTRANSFORMFEEDBACKINSTANCED: {
                int mode = *pb++;
                int id = *pb++;
                int instancecount = *pb++;
                glDrawTransformFeedbackInstanced(mode, id, instancecount);
                break;
            }
            case SGL_CMD_DRAWTRANSFORMFEEDBACKSTREAMINSTANCED: {
                int mode = *pb++;
                int id = *pb++;
                int stream = *pb++;
                int instancecount = *pb++;
                glDrawTransformFeedbackStreamInstanced(mode, id, stream, instancecount);
                break;
            }
            case SGL_CMD_DISPATCHCOMPUTEINDIRECT: {
                int indirect = *pb++;
                glDispatchComputeIndirect(indirect);
                break;
            }
            case SGL_CMD_COPYIMAGESUBDATA: {
                int srcName = *pb++;
                int srcTarget = *pb++;
                int srcLevel = *pb++;
                int srcX = *pb++;
                int srcY = *pb++;
                int srcZ = *pb++;
                int dstName = *pb++;
                int dstTarget = *pb++;
                int dstLevel = *pb++;
                int dstX = *pb++;
                int dstY = *pb++;
                int dstZ = *pb++;
                int srcWidth = *pb++;
                int srcHeight = *pb++;
                int srcDepth = *pb++;
                glCopyImageSubData(srcName, srcTarget, srcLevel, srcX, srcY, srcZ, dstName, dstTarget, dstLevel, dstX, dstY, dstZ, srcWidth, srcHeight, srcDepth);
                break;
            }
            case SGL_CMD_FRAMEBUFFERPARAMETERI: {
                int target = *pb++;
                int pname = *pb++;
                int param = *pb++;
                glFramebufferParameteri(target, pname, param);
                break;
            }
            case SGL_CMD_INVALIDATETEXSUBIMAGE: {
                int texture = *pb++;
                int level = *pb++;
                int xoffset = *pb++;
                int yoffset = *pb++;
                int zoffset = *pb++;
                int width = *pb++;
                int height = *pb++;
                int depth = *pb++;
                glInvalidateTexSubImage(texture, level, xoffset, yoffset, zoffset, width, height, depth);
                break;
            }
            case SGL_CMD_INVALIDATETEXIMAGE: {
                int texture = *pb++;
                int level = *pb++;
                glInvalidateTexImage(texture, level);
                break;
            }
            case SGL_CMD_INVALIDATEBUFFERSUBDATA: {
                int buffer = *pb++;
                int offset = *pb++;
                int length = *pb++;
                glInvalidateBufferSubData(buffer, offset, length);
                break;
            }
            case SGL_CMD_INVALIDATEBUFFERDATA: {
                int buffer = *pb++;
                glInvalidateBufferData(buffer);
                break;
            }
            case SGL_CMD_SHADERSTORAGEBLOCKBINDING: {
                int program = *pb++;
                int storageBlockIndex = *pb++;
                int storageBlockBinding = *pb++;
                glShaderStorageBlockBinding(program, storageBlockIndex, storageBlockBinding);
                break;
            }
            case SGL_CMD_TEXBUFFERRANGE: {
                int target = *pb++;
                int internalformat = *pb++;
                int buffer = *pb++;
                int offset = *pb++;
                int size = *pb++;
                glTexBufferRange(target, internalformat, buffer, offset, size);
                break;
            }
            case SGL_CMD_TEXSTORAGE2DMULTISAMPLE: {
                int target = *pb++;
                int samples = *pb++;
                int internalformat = *pb++;
                int width = *pb++;
                int height = *pb++;
                int fixedsamplelocations = *pb++;
                glTexStorage2DMultisample(target, samples, internalformat, width, height, fixedsamplelocations);
                break;
            }
            case SGL_CMD_TEXSTORAGE3DMULTISAMPLE: {
                int target = *pb++;
                int samples = *pb++;
                int internalformat = *pb++;
                int width = *pb++;
                int height = *pb++;
                int depth = *pb++;
                int fixedsamplelocations = *pb++;
                glTexStorage3DMultisample(target, samples, internalformat, width, height, depth, fixedsamplelocations);
                break;
            }
            case SGL_CMD_TEXTUREVIEW: {
                int texture = *pb++;
                int target = *pb++;
                int origtexture = *pb++;
                int internalformat = *pb++;
                int minlevel = *pb++;
                int numlevels = *pb++;
                int minlayer = *pb++;
                int numlayers = *pb++;
                glTextureView(texture, target, origtexture, internalformat, minlevel, numlevels, minlayer, numlayers);
                break;
            }
            case SGL_CMD_BINDVERTEXBUFFER: {
                int bindingindex = *pb++;
                int buffer = *pb++;
                int offset = *pb++;
                int stride = *pb++;
                glBindVertexBuffer(bindingindex, buffer, offset, stride);
                break;
            }
            case SGL_CMD_VERTEXATTRIBFORMAT: {
                int attribindex = *pb++;
                int size = *pb++;
                int type = *pb++;
                int normalized = *pb++;
                int relativeoffset = *pb++;
                glVertexAttribFormat(attribindex, size, type, normalized, relativeoffset);
                break;
            }
            case SGL_CMD_VERTEXATTRIBIFORMAT: {
                int attribindex = *pb++;
                int size = *pb++;
                int type = *pb++;
                int relativeoffset = *pb++;
                glVertexAttribIFormat(attribindex, size, type, relativeoffset);
                break;
            }
            case SGL_CMD_VERTEXATTRIBLFORMAT: {
                int attribindex = *pb++;
                int size = *pb++;
                int type = *pb++;
                int relativeoffset = *pb++;
                glVertexAttribLFormat(attribindex, size, type, relativeoffset);
                break;
            }
            case SGL_CMD_VERTEXATTRIBBINDING: {
                int attribindex = *pb++;
                int bindingindex = *pb++;
                glVertexAttribBinding(attribindex, bindingindex);
                break;
            }
            case SGL_CMD_VERTEXBINDINGDIVISOR: {
                int bindingindex = *pb++;
                int divisor = *pb++;
                glVertexBindingDivisor(bindingindex, divisor);
                break;
            }
            case SGL_CMD_POPDEBUGGROUP: {
                glPopDebugGroup();
                break;
            }
            case SGL_CMD_CLIPCONTROL: {
                int origin = *pb++;
                int depth = *pb++;
                glClipControl(origin, depth);
                break;
            }
            case SGL_CMD_TRANSFORMFEEDBACKBUFFERBASE: {
                int xfb = *pb++;
                int index = *pb++;
                int buffer = *pb++;
                glTransformFeedbackBufferBase(xfb, index, buffer);
                break;
            }
            case SGL_CMD_TRANSFORMFEEDBACKBUFFERRANGE: {
                int xfb = *pb++;
                int index = *pb++;
                int buffer = *pb++;
                int offset = *pb++;
                int size = *pb++;
                glTransformFeedbackBufferRange(xfb, index, buffer, offset, size);
                break;
            }
            case SGL_CMD_COPYNAMEDBUFFERSUBDATA: {
                int readBuffer = *pb++;
                int writeBuffer = *pb++;
                int readOffset = *pb++;
                int writeOffset = *pb++;
                int size = *pb++;
                glCopyNamedBufferSubData(readBuffer, writeBuffer, readOffset, writeOffset, size);
                break;
            }
            case SGL_CMD_UNMAPNAMEDBUFFER: {
                int buffer = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glUnmapNamedBuffer(buffer);
                break;
            }
            case SGL_CMD_FLUSHMAPPEDNAMEDBUFFERRANGE: {
                int buffer = *pb++;
                int offset = *pb++;
                int length = *pb++;
                glFlushMappedNamedBufferRange(buffer, offset, length);
                break;
            }
            case SGL_CMD_NAMEDFRAMEBUFFERRENDERBUFFER: {
                int framebuffer = *pb++;
                int attachment = *pb++;
                int renderbuffertarget = *pb++;
                int renderbuffer = *pb++;
                glNamedFramebufferRenderbuffer(framebuffer, attachment, renderbuffertarget, renderbuffer);
                break;
            }
            case SGL_CMD_NAMEDFRAMEBUFFERPARAMETERI: {
                int framebuffer = *pb++;
                int pname = *pb++;
                int param = *pb++;
                glNamedFramebufferParameteri(framebuffer, pname, param);
                break;
            }
            case SGL_CMD_NAMEDFRAMEBUFFERTEXTURE: {
                int framebuffer = *pb++;
                int attachment = *pb++;
                int texture = *pb++;
                int level = *pb++;
                glNamedFramebufferTexture(framebuffer, attachment, texture, level);
                break;
            }
            case SGL_CMD_NAMEDFRAMEBUFFERTEXTURELAYER: {
                int framebuffer = *pb++;
                int attachment = *pb++;
                int texture = *pb++;
                int level = *pb++;
                int layer = *pb++;
                glNamedFramebufferTextureLayer(framebuffer, attachment, texture, level, layer);
                break;
            }
            case SGL_CMD_NAMEDFRAMEBUFFERDRAWBUFFER: {
                int framebuffer = *pb++;
                int buf = *pb++;
                glNamedFramebufferDrawBuffer(framebuffer, buf);
                break;
            }
            case SGL_CMD_NAMEDFRAMEBUFFERREADBUFFER: {
                int framebuffer = *pb++;
                int src = *pb++;
                glNamedFramebufferReadBuffer(framebuffer, src);
                break;
            }
            case SGL_CMD_CLEARNAMEDFRAMEBUFFERFI: {
                int framebuffer = *pb++;
                int buffer = *pb++;
                int drawbuffer = *pb++;
                float depth = *((float*)pb++);
                int stencil = *pb++;
                glClearNamedFramebufferfi(framebuffer, buffer, drawbuffer, depth, stencil);
                break;
            }
            case SGL_CMD_BLITNAMEDFRAMEBUFFER: {
                int readFramebuffer = *pb++;
                int drawFramebuffer = *pb++;
                int srcX0 = *pb++;
                int srcY0 = *pb++;
                int srcX1 = *pb++;
                int srcY1 = *pb++;
                int dstX0 = *pb++;
                int dstY0 = *pb++;
                int dstX1 = *pb++;
                int dstY1 = *pb++;
                int mask = *pb++;
                int filter = *pb++;
                glBlitNamedFramebuffer(readFramebuffer, drawFramebuffer, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
                break;
            }
            case SGL_CMD_CHECKNAMEDFRAMEBUFFERSTATUS: {
                int framebuffer = *pb++;
                int target = *pb++;
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glCheckNamedFramebufferStatus(framebuffer, target);
                break;
            }
            case SGL_CMD_NAMEDRENDERBUFFERSTORAGE: {
                int renderbuffer = *pb++;
                int internalformat = *pb++;
                int width = *pb++;
                int height = *pb++;
                glNamedRenderbufferStorage(renderbuffer, internalformat, width, height);
                break;
            }
            case SGL_CMD_NAMEDRENDERBUFFERSTORAGEMULTISAMPLE: {
                int renderbuffer = *pb++;
                int samples = *pb++;
                int internalformat = *pb++;
                int width = *pb++;
                int height = *pb++;
                glNamedRenderbufferStorageMultisample(renderbuffer, samples, internalformat, width, height);
                break;
            }
            case SGL_CMD_TEXTUREBUFFER: {
                int texture = *pb++;
                int internalformat = *pb++;
                int buffer = *pb++;
                glTextureBuffer(texture, internalformat, buffer);
                break;
            }
            case SGL_CMD_TEXTUREBUFFERRANGE: {
                int texture = *pb++;
                int internalformat = *pb++;
                int buffer = *pb++;
                int offset = *pb++;
                int size = *pb++;
                glTextureBufferRange(texture, internalformat, buffer, offset, size);
                break;
            }
            case SGL_CMD_TEXTURESTORAGE1D: {
                int texture = *pb++;
                int levels = *pb++;
                int internalformat = *pb++;
                int width = *pb++;
                glTextureStorage1D(texture, levels, internalformat, width);
                break;
            }
            case SGL_CMD_TEXTURESTORAGE2D: {
                int texture = *pb++;
                int levels = *pb++;
                int internalformat = *pb++;
                int width = *pb++;
                int height = *pb++;
                glTextureStorage2D(texture, levels, internalformat, width, height);
                break;
            }
            case SGL_CMD_TEXTURESTORAGE3D: {
                int texture = *pb++;
                int levels = *pb++;
                int internalformat = *pb++;
                int width = *pb++;
                int height = *pb++;
                int depth = *pb++;
                glTextureStorage3D(texture, levels, internalformat, width, height, depth);
                break;
            }
            case SGL_CMD_TEXTURESTORAGE2DMULTISAMPLE: {
                int texture = *pb++;
                int samples = *pb++;
                int internalformat = *pb++;
                int width = *pb++;
                int height = *pb++;
                int fixedsamplelocations = *pb++;
                glTextureStorage2DMultisample(texture, samples, internalformat, width, height, fixedsamplelocations);
                break;
            }
            case SGL_CMD_TEXTURESTORAGE3DMULTISAMPLE: {
                int texture = *pb++;
                int samples = *pb++;
                int internalformat = *pb++;
                int width = *pb++;
                int height = *pb++;
                int depth = *pb++;
                int fixedsamplelocations = *pb++;
                glTextureStorage3DMultisample(texture, samples, internalformat, width, height, depth, fixedsamplelocations);
                break;
            }
            case SGL_CMD_COPYTEXTURESUBIMAGE1D: {
                int texture = *pb++;
                int level = *pb++;
                int xoffset = *pb++;
                int x = *pb++;
                int y = *pb++;
                int width = *pb++;
                glCopyTextureSubImage1D(texture, level, xoffset, x, y, width);
                break;
            }
            case SGL_CMD_COPYTEXTURESUBIMAGE2D: {
                int texture = *pb++;
                int level = *pb++;
                int xoffset = *pb++;
                int yoffset = *pb++;
                int x = *pb++;
                int y = *pb++;
                int width = *pb++;
                int height = *pb++;
                glCopyTextureSubImage2D(texture, level, xoffset, yoffset, x, y, width, height);
                break;
            }
            case SGL_CMD_COPYTEXTURESUBIMAGE3D: {
                int texture = *pb++;
                int level = *pb++;
                int xoffset = *pb++;
                int yoffset = *pb++;
                int zoffset = *pb++;
                int x = *pb++;
                int y = *pb++;
                int width = *pb++;
                int height = *pb++;
                glCopyTextureSubImage3D(texture, level, xoffset, yoffset, zoffset, x, y, width, height);
                break;
            }
            case SGL_CMD_TEXTUREPARAMETERF: {
                int texture = *pb++;
                int pname = *pb++;
                float param = *((float*)pb++);
                glTextureParameterf(texture, pname, param);
                break;
            }
            case SGL_CMD_TEXTUREPARAMETERI: {
                int texture = *pb++;
                int pname = *pb++;
                int param = *pb++;
                glTextureParameteri(texture, pname, param);
                break;
            }
            case SGL_CMD_GENERATETEXTUREMIPMAP: {
                int texture = *pb++;
                glGenerateTextureMipmap(texture);
                break;
            }
            case SGL_CMD_BINDTEXTUREUNIT: {
                int unit = *pb++;
                int texture = *pb++;
                glBindTextureUnit(unit, texture);
                break;
            }
            case SGL_CMD_DISABLEVERTEXARRAYATTRIB: {
                int vaobj = *pb++;
                int index = *pb++;
                glDisableVertexArrayAttrib(vaobj, index);
                break;
            }
            case SGL_CMD_ENABLEVERTEXARRAYATTRIB: {
                int vaobj = *pb++;
                int index = *pb++;
                glEnableVertexArrayAttrib(vaobj, index);
                break;
            }
            case SGL_CMD_VERTEXARRAYELEMENTBUFFER: {
                int vaobj = *pb++;
                int buffer = *pb++;
                glVertexArrayElementBuffer(vaobj, buffer);
                break;
            }
            case SGL_CMD_VERTEXARRAYVERTEXBUFFER: {
                int vaobj = *pb++;
                int bindingindex = *pb++;
                int buffer = *pb++;
                int offset = *pb++;
                int stride = *pb++;
                glVertexArrayVertexBuffer(vaobj, bindingindex, buffer, offset, stride);
                break;
            }
            case SGL_CMD_VERTEXARRAYATTRIBBINDING: {
                int vaobj = *pb++;
                int attribindex = *pb++;
                int bindingindex = *pb++;
                glVertexArrayAttribBinding(vaobj, attribindex, bindingindex);
                break;
            }
            case SGL_CMD_VERTEXARRAYATTRIBFORMAT: {
                int vaobj = *pb++;
                int attribindex = *pb++;
                int size = *pb++;
                int type = *pb++;
                int normalized = *pb++;
                int relativeoffset = *pb++;
                glVertexArrayAttribFormat(vaobj, attribindex, size, type, normalized, relativeoffset);
                break;
            }
            case SGL_CMD_VERTEXARRAYATTRIBIFORMAT: {
                int vaobj = *pb++;
                int attribindex = *pb++;
                int size = *pb++;
                int type = *pb++;
                int relativeoffset = *pb++;
                glVertexArrayAttribIFormat(vaobj, attribindex, size, type, relativeoffset);
                break;
            }
            case SGL_CMD_VERTEXARRAYATTRIBLFORMAT: {
                int vaobj = *pb++;
                int attribindex = *pb++;
                int size = *pb++;
                int type = *pb++;
                int relativeoffset = *pb++;
                glVertexArrayAttribLFormat(vaobj, attribindex, size, type, relativeoffset);
                break;
            }
            case SGL_CMD_VERTEXARRAYBINDINGDIVISOR: {
                int vaobj = *pb++;
                int bindingindex = *pb++;
                int divisor = *pb++;
                glVertexArrayBindingDivisor(vaobj, bindingindex, divisor);
                break;
            }
            case SGL_CMD_GETQUERYBUFFEROBJECTI64V: {
                int id = *pb++;
                int buffer = *pb++;
                int pname = *pb++;
                int offset = *pb++;
                glGetQueryBufferObjecti64v(id, buffer, pname, offset);
                break;
            }
            case SGL_CMD_GETQUERYBUFFEROBJECTIV: {
                int id = *pb++;
                int buffer = *pb++;
                int pname = *pb++;
                int offset = *pb++;
                glGetQueryBufferObjectiv(id, buffer, pname, offset);
                break;
            }
            case SGL_CMD_GETQUERYBUFFEROBJECTUI64V: {
                int id = *pb++;
                int buffer = *pb++;
                int pname = *pb++;
                int offset = *pb++;
                glGetQueryBufferObjectui64v(id, buffer, pname, offset);
                break;
            }
            case SGL_CMD_GETQUERYBUFFEROBJECTUIV: {
                int id = *pb++;
                int buffer = *pb++;
                int pname = *pb++;
                int offset = *pb++;
                glGetQueryBufferObjectuiv(id, buffer, pname, offset);
                break;
            }
            case SGL_CMD_MEMORYBARRIERBYREGION: {
                int barriers = *pb++;
                glMemoryBarrierByRegion(barriers);
                break;
            }
            case SGL_CMD_GETGRAPHICSRESETSTATUS: {
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = glGetGraphicsResetStatus();
                break;
            }
            case SGL_CMD_TEXTUREBARRIER: {
                glTextureBarrier();
                break;
            }
            case SGL_CMD_POLYGONOFFSETCLAMP: {
                float factor = *((float*)pb++);
                float units = *((float*)pb++);
                float clamp = *((float*)pb++);
                glPolygonOffsetClamp(factor, units, clamp);
                break;
            }
            case SGL_CMD_POLYGONSTIPPLE: {
                unsigned char *mask = (unsigned char*)pb;
                glPolygonStipple(mask);
                pb += 32 * 32;
                break;
            }
            case SGL_CMD_FEEDBACKBUFFER: {
                int size = *pb++,
                    type = *pb++;
                glFeedbackBuffer(size, type, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_SELECTBUFFER: {
                int size = *pb++;
                glSelectBuffer(size, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_MAP1F: {
                int target = *pb++;
                float u1 = *((float*)pb++);
                float u2 = *((float*)pb++);
                int stride = *pb++;
                int order = *pb++;
                glMap1f(target, u1, u2, stride, order, uploaded);
                break;
            }
            case SGL_CMD_PIXELMAPFV: {
                int map = *pb++,
                    mapsize = *pb++;
                glPixelMapfv(map, mapsize, uploaded);
                break;
            }
            case SGL_CMD_PIXELMAPUIV: {
                int map = *pb++,
                    mapsize = *pb++;
                glPixelMapuiv(map, mapsize, uploaded);
                break;
            }
            case SGL_CMD_GETCLIPPLANE: {
                int plane = *pb++,
                    is_float = *pb++;
                if (is_float)
                    glGetClipPlanef(plane, p + SGL_OFFSET_REGISTER_RETVAL_V);
                else
                    glGetClipPlane(plane, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_GETLIGHTFV: {
                int light = *pb++,
                    pname = *pb++;
                float v[4];
                glGetLightfv(light, pname, v);
                memcpy(p + SGL_OFFSET_REGISTER_RETVAL_V, v, 4 * sizeof(float));
                break;
            }
            case SGL_CMD_GETLIGHTIV: {
                int light = *pb++,
                    pname = *pb++;
                int v[4];
                glGetLightiv(light, pname, v);
                memcpy(p + SGL_OFFSET_REGISTER_RETVAL_V, v, 4 * sizeof(int));
                break;
            }
            case SGL_CMD_ARETEXTURESRESIDENT: {
                unsigned int tex = *pb++;
                GLboolean res;
                glAreTexturesResident(1, &tex, &res);
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = res;
                break;
            }
            case SGL_CMD_PRIORITIZETEXTURES: {
                unsigned int tex = *pb++;
                float priority = *((float*)pb++);
                glPrioritizeTextures(1, &tex, &priority);
                break;
            }
            case SGL_CMD_COMPRESSEDTEXIMAGE3D: {
                int target = *pb++,
                    level = *pb++,
                    internalformat = *pb++,
                    width = *pb++,
                    height = *pb++,
                    depth = *pb++,
                    border = *pb++,
                    imageSize = *pb++;
                glCompressedTexImage3D(target, level, internalformat, width, height, depth, border, imageSize, uploaded);
                break;
            }
            case SGL_CMD_COMPRESSEDTEXIMAGE2D: {
                int target = *pb++,
                    level = *pb++,
                    internalformat = *pb++,
                    width = *pb++,
                    height = *pb++,
                    border = *pb++,
                    imageSize = *pb++;
                glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, uploaded);
                break;
            }
            case SGL_CMD_COMPRESSEDTEXIMAGE1D: {
                int target = *pb++,
                    level = *pb++,
                    internalformat = *pb++,
                    width = *pb++,
                    border = *pb++,
                    imageSize = *pb++;
                glCompressedTexImage1D(target, level, internalformat, width, border, imageSize, uploaded);
                break;
            }
            case SGL_CMD_COMPRESSEDTEXSUBIMAGE3D: {
                int target = *pb++,
                    level = *pb++,
                    xoffset = *pb++,
                    yoffset = *pb++,
                    zoffset = *pb++,
                    width = *pb++,
                    height = *pb++,
                    depth = *pb++,
                    format = *pb++,
                    imageSize = *pb++;
                glCompressedTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, imageSize, uploaded);
                break;
            }
            case SGL_CMD_COMPRESSEDTEXSUBIMAGE2D: {
                int target = *pb++,
                    level = *pb++,
                    xoffset = *pb++,
                    yoffset = *pb++,
                    zoffset = *pb++,
                    width = *pb++,
                    height = *pb++,
                    depth = *pb++,
                    format = *pb++,
                    imageSize = *pb++;
                glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, imageSize, uploaded);
                break;
            }
            case SGL_CMD_COMPRESSEDTEXSUBIMAGE1D: {
                int target = *pb++,
                    level = *pb++,
                    xoffset = *pb++,
                    yoffset = *pb++,
                    zoffset = *pb++,
                    width = *pb++,
                    format = *pb++,
                    imageSize = *pb++;
                glCompressedTexSubImage1D(target, level, xoffset, width, format, imageSize, uploaded);
                break;
            }
            case SGL_CMD_LOADTRANSPOSEMATRIXF: {
                float m[16];
                for (int i = 0; i < 16; i++)
                    m[i] = *((float*)pb++);
                glLoadTransposeMatrixf(m);
                break;
            }
            case SGL_CMD_MULTTRANSPOSEMATRIXF: {
                float m[16];
                for (int i = 0; i < 16; i++)
                    m[i] = *((float*)pb++);
                glMultTransposeMatrixf(m);
                break;
            }
            case SGL_CMD_DELETEQUERIES: {
                unsigned int id = *pb++;
                glDeleteQueries(1, &id);
                break;
            }
            case SGL_CMD_GETQUERYIV: {
                int target = *pb++,
                    pname = *pb++;
                int params;
                glGetQueryiv(target, pname, &params);
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = params;
                break;
            }
            case SGL_CMD_GETQUERYOBJECTIV: {
                int id = *pb++,
                    pname = *pb++;
                int params;
                glGetQueryObjectiv(id, pname, &params);
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = params;
                break;
            }
            case SGL_CMD_GETQUERYOBJECTUIV: {
                int id = *pb++,
                    pname = *pb++;
                unsigned int params;
                glGetQueryObjectuiv(id, pname, &params);
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = params;
                break;
            }
            case SGL_CMD_BUFFERSUBDATA: {
                int target = *pb++,
                    offset = *pb++,
                    size = *pb++;
                glBufferSubData(target, offset, size, uploaded);
                break;
            }
            case SGL_CMD_GETBUFFERPARAMETERIV: {
                int target = *pb++,
                    pname = *pb++;
                int params;
                glGetBufferParameteriv(target, pname, &params);
                *(int*)(p + SGL_OFFSET_REGISTER_RETVAL) = params;
                break;
            }
            case SGL_CMD_GETMATERIALFV: {
                int face = *pb++,
                    pname = *pb++;
                glGetMaterialfv(face, pname, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_GETMATERIALIV: {
                int face = *pb++,
                    pname = *pb++;
                glGetMaterialfv(face, pname, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_GETTEXENVFV: {
                int target = *pb++,
                    pname = *pb++;
                glGetTexEnvfv(target, pname, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_GETTEXENVIV: {
                int target = *pb++,
                    pname = *pb++;
                glGetTexEnviv(target, pname, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_GETTEXGENDV: {
                int coord = *pb++,
                    pname = *pb++;
                glGetTexGendv(coord, pname, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_GETTEXGENFV: {
                int coord = *pb++,
                    pname = *pb++;
                glGetTexGenfv(coord, pname, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_GETTEXGENIV: {
                int coord = *pb++,
                    pname = *pb++;
                glGetTexGeniv(coord, pname, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_TEXPARAMETERFV: {
                int target = *pb++,
                    pname = *pb++;
                float params[4];
                params[0] = *((float*)pb++);
                params[1] = *((float*)pb++);
                params[2] = *((float*)pb++);
                params[3] = *((float*)pb++);
                glTexParameterfv(target, pname, params);
                break;
            }
            case SGL_CMD_TEXPARAMETERIV: {
                int target = *pb++,
                    pname = *pb++;
                int params[4];
                params[0] = *pb++;
                params[1] = *pb++;
                params[2] = *pb++;
                params[3] = *pb++;
                glTexParameteriv(target, pname, params);
                break;
            }
            case SGL_CMD_GETTEXPARAMETERFV: {
                int target = *pb++,
                    pname = *pb++;
                glGetTexParameterfv(target, pname, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_GETTEXPARAMETERIV: {
                int target = *pb++,
                    pname = *pb++;
                glGetTexParameteriv(target, pname, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_GETTEXLEVELPARAMETERFV: {
                int target = *pb++,
                    level = *pb++,
                    pname = *pb++;
                glGetTexLevelParameterfv(target, level, pname, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_GETTEXLEVELPARAMETERIV: {
                int target = *pb++,
                    level = *pb++,
                    pname = *pb++;
                glGetTexLevelParameteriv(target, level, pname, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_BINDATTRIBLOCATION: {
                int program = *pb++,
                    index = *pb++;
                char *name = (char*)pb;
                glBindAttribLocation(program, index, name);
                ADVANCE_PAST_STRING();
                break;
            }
            case SGL_CMD_GETACTIVEATTRIB: {
                int program = *pb++,
                    index = *pb++,
                    bufSize = *pb++;
                glGetActiveAttrib(
                    program,
                    index,
                    bufSize,
                    p + SGL_OFFSET_REGISTER_RETVAL_V,
                    p + SGL_OFFSET_REGISTER_RETVAL_V + sizeof(GLsizei),
                    p + SGL_OFFSET_REGISTER_RETVAL_V + sizeof(GLsizei) + sizeof(GLint),
                    p + SGL_OFFSET_REGISTER_RETVAL_V + sizeof(GLsizei) + sizeof(GLint) + sizeof(GLenum)
                );
                break;
            }
            case SGL_CMD_GETACTIVEUNIFORM: {
                int program = *pb++,
                    index = *pb++,
                    bufSize = *pb++;
                glGetActiveUniform(
                    program,
                    index,
                    bufSize,
                    p + SGL_OFFSET_REGISTER_RETVAL_V,
                    p + SGL_OFFSET_REGISTER_RETVAL_V + sizeof(GLsizei),
                    p + SGL_OFFSET_REGISTER_RETVAL_V + sizeof(GLsizei) + sizeof(GLint),
                    p + SGL_OFFSET_REGISTER_RETVAL_V + sizeof(GLsizei) + sizeof(GLint) + sizeof(GLenum)
                );
                break;
            }
            case SGL_CMD_GETATTACHEDSHADERS: {
                int program = *pb++,
                    maxCount = *pb++;
                glGetAttachedShaders(program, maxCount,  p + SGL_OFFSET_REGISTER_RETVAL_V,
                    p + SGL_OFFSET_REGISTER_RETVAL_V + sizeof(GLsizei));
                break;
            }
            case SGL_CMD_GETPROGRAMINFOLOG: {
                int program = *pb++,
                    bufSize = *pb++;
                glGetProgramInfoLog(program, bufSize,  p + SGL_OFFSET_REGISTER_RETVAL_V,
                    p + SGL_OFFSET_REGISTER_RETVAL_V + sizeof(GLsizei));
                break;
            }
            case SGL_CMD_GETSHADERINFOLOG: {
                int program = *pb++,
                    bufSize = *pb++;
                glGetShaderInfoLog(program, bufSize,  p + SGL_OFFSET_REGISTER_RETVAL_V,
                    p + SGL_OFFSET_REGISTER_RETVAL_V + sizeof(GLsizei));
                break;
            }
            case SGL_CMD_GETSHADERSOURCE: {
                int shader = *pb++,
                    bufSize = *pb++;
                glGetShaderSource(shader, bufSize,  p + SGL_OFFSET_REGISTER_RETVAL_V,
                    p + SGL_OFFSET_REGISTER_RETVAL_V + sizeof(GLsizei));
                break;
            }
            case SGL_CMD_GETUNIFORMFV: {
                int program = *pb++,
                    location = *pb++;
                glGetUniformfv(program, location, p + SGL_OFFSET_REGISTER_RETVAL);
                break;
            }
            case SGL_CMD_GETUNIFORMIV: {
                int program = *pb++,
                    location = *pb++;
                glGetUniformiv(program, location, p + SGL_OFFSET_REGISTER_RETVAL);
                break;
            }
            case SGL_CMD_GETVERTEXATTRIBDV: {
                int index = *pb++,
                    pname = *pb++;
                glGetVertexAttribdv(index, pname, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_GETVERTEXATTRIBFV: {
                int index = *pb++,
                    pname = *pb++;
                glGetVertexAttribfv(index, pname, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_GETVERTEXATTRIBIV: {
                int index = *pb++,
                    pname = *pb++;
                glGetVertexAttribiv(index, pname, p + SGL_OFFSET_REGISTER_RETVAL_V);
                break;
            }
            case SGL_CMD_UNIFORMMATRIX2FV: {
                int location = *pb++,
                    count = *pb++,
                    transpose = *pb++;
                glUniformMatrix2fv(location, count, transpose, uploaded);
                break;
            }
            case SGL_CMD_UNIFORMMATRIX3FV: {
                int location = *pb++,
                    count = *pb++,
                    transpose = *pb++;
                glUniformMatrix3fv(location, count, transpose, uploaded);
                break;
            }
            case SGL_CMD_VERTEXATTRIB4NBV: {
                int index = *pb++;
                GLbyte v[4];
                v[0] = *pb++;
                v[1] = *pb++;
                v[2] = *pb++;
                v[3] = *pb++;
                glVertexAttrib4Nbv(index, v);
                break;
            }
            case SGL_CMD_VERTEXATTRIB4NIV: {
                int index = *pb++;
                GLint v[4];
                v[0] = *pb++;
                v[1] = *pb++;
                v[2] = *pb++;
                v[3] = *pb++;
                glVertexAttrib4Niv(index, v);
                break;
            }
            case SGL_CMD_VERTEXATTRIB4NSV: {
                int index = *pb++;
                GLshort v[4];
                v[0] = *pb++;
                v[1] = *pb++;
                v[2] = *pb++;
                v[3] = *pb++;
                glVertexAttrib4Nsv(index, v);
                break;
            }
            case SGL_CMD_VERTEXATTRIB4NUIV: {
                int index = *pb++;
                GLuint v[4];
                v[0] = *pb++;
                v[1] = *pb++;
                v[2] = *pb++;
                v[3] = *pb++;
                glVertexAttrib4Nuiv(index, v);
                break;
            }
            case SGL_CMD_VERTEXATTRIB4NUSV: {
                int index = *pb++;
                GLushort v[4];
                v[0] = *pb++;
                v[1] = *pb++;
                v[2] = *pb++;
                v[3] = *pb++;
                glVertexAttrib4Nusv(index, v);
                break;
            }
            case SGL_CMD_VERTEXATTRIB4BV: {
                int index = *pb++;
                GLbyte v[4];
                v[0] = *pb++;
                v[1] = *pb++;
                v[2] = *pb++;
                v[3] = *pb++;
                glVertexAttrib4bv(index, v);
                break;
            }
            case SGL_CMD_VERTEXATTRIB4IV: {
                int index = *pb++;
                GLint v[4];
                v[0] = *pb++;
                v[1] = *pb++;
                v[2] = *pb++;
                v[3] = *pb++;
                glVertexAttrib4iv(index, v);
                break;
            }
            case SGL_CMD_VERTEXATTRIB4SV: {
                int index = *pb++;
                GLshort v[4];
                v[0] = *pb++;
                v[1] = *pb++;
                v[2] = *pb++;
                v[3] = *pb++;
                glVertexAttrib4sv(index, v);
                break;
            }
            case SGL_CMD_VERTEXATTRIB4UIV: {
                int index = *pb++;
                GLuint v[4];
                v[0] = *pb++;
                v[1] = *pb++;
                v[2] = *pb++;
                v[3] = *pb++;
                glVertexAttrib4uiv(index, v);
                break;
            }
            case SGL_CMD_VERTEXATTRIB4USV: {
                int index = *pb++;
                GLushort v[4];
                v[0] = *pb++;
                v[1] = *pb++;
                v[2] = *pb++;
                v[3] = *pb++;
                glVertexAttrib4usv(index, v);
                break;
            }
            case SGL_CMD_UNIFORMMATRIX2X3FV: {
                int location = *pb++,
                    count = *pb++,
                    transpose = *pb++;
                glUniformMatrix2x3fv(location, count, transpose, uploaded);
                break;
            }
            case SGL_CMD_UNIFORMMATRIX3X2FV: {
                int location = *pb++,
                    count = *pb++,
                    transpose = *pb++;
                glUniformMatrix3x2fv(location, count, transpose, uploaded);
                break;
            }
            case SGL_CMD_UNIFORMMATRIX2X4FV: {
                int location = *pb++,
                    count = *pb++,
                    transpose = *pb++;
                glUniformMatrix2x4fv(location, count, transpose, uploaded);
                break;
            }
            case SGL_CMD_UNIFORMMATRIX4X2FV: {
                int location = *pb++,
                    count = *pb++,
                    transpose = *pb++;
                glUniformMatrix4x2fv(location, count, transpose, uploaded);
                break;
            }
            case SGL_CMD_UNIFORMMATRIX3X4FV: {
                int location = *pb++,
                    count = *pb++,
                    transpose = *pb++;
                glUniformMatrix3x4fv(location, count, transpose, uploaded);
                break;
            }
            case SGL_CMD_UNIFORMMATRIX4X3FV: {
                int location = *pb++,
                    count = *pb++,
                    transpose = *pb++;
                glUniformMatrix4x3fv(location, count, transpose, uploaded);
                break;
            }
            case SGL_CMD_CLEARBUFFERIV: {
                int buffer = *pb++,
                    drawbuffer = *pb++;
                int value[4];
                for (int i = 0; i < (buffer == GL_COLOR ? 4 : 1); i++)
                    value[i] = *pb++;
                glClearBufferiv(buffer, drawbuffer, value);
                break;
            }
            case SGL_CMD_CLEARBUFFERUIV: {
                int buffer = *pb++,
                    drawbuffer = *pb++;
                unsigned int value[4];
                for (int i = 0; i < (buffer == GL_COLOR ? 4 : 1); i++)
                    value[i] = *pb++;
                glClearBufferuiv(buffer, drawbuffer, value);
                break;
            }
            case SGL_CMD_CLEARBUFFERFV: {
                int buffer = *pb++,
                    drawbuffer = *pb++;
                float value[4];
                for (int i = 0; i < (buffer == GL_COLOR ? 4 : 1); i++)
                    value[i] = *((float*)pb++);
                glClearBufferfv(buffer, drawbuffer, value);
                break;
            }
            }
            if (!begun) {
                int error = glGetError();
                if (error != GL_NO_ERROR)
                    printf("%sglerr%s: client %s%d%s opengl error: %s%d%s (%s0x%04x%s) from %s%s%s (%s%d%s)\n", 
                        COLOR(COLOR_ATTR_BOLD COLOR_FG_RED COLOR_BG_NONE), COLOR(COLOR_RESET), 
                        COLOR(COLOR_ATTR_BOLD COLOR_FG_GREEN COLOR_BG_NONE), client_id, COLOR(COLOR_RESET), 
                        COLOR(COLOR_ATTR_BOLD COLOR_FG_GREEN COLOR_BG_NONE), error, COLOR(COLOR_RESET), 
                        COLOR(COLOR_ATTR_BOLD COLOR_FG_GREEN COLOR_BG_NONE), error, COLOR(COLOR_RESET), 
                        COLOR(COLOR_ATTR_BOLD COLOR_FG_BLUE COLOR_BG_NONE), cmd < SGL_CMD_MAX ? SGL_CMD_STRING_TABLE[cmd] : "????", COLOR(COLOR_RESET), 
                        COLOR(COLOR_ATTR_BOLD COLOR_FG_GREEN COLOR_BG_NONE), cmd, COLOR(COLOR_RESET)
                    );
            }
        }

        /* commit done */
        glFinish();
        *(int*)(p + SGL_OFFSET_REGISTER_COMMIT) = 0;
    }
}