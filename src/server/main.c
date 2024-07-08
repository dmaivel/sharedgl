#define SHAREDGL_HOST

#include <sharedgl.h>
#include <sgldebug.h>

#include <server/processor.h>
#include <server/overlay.h>
#include <server/context.h>

#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>

#include <sys/mman.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>

static void *shm_ptr;
static size_t shm_size;

static int *internal_cmd_ptr;

static const char *usage =
    "usage: sglrenderer [-h] [-v] [-o] [-n] [-x] [-g MAJOR.MINOR] [-r WIDTHxHEIGHT] [-m SIZE] [-p PORT]\n"
    "\n"
    "options:\n"
    "    -h                 display help information\n"
    "    -v                 display virtual machine arguments\n"
    "    -o                 enables fps overlay on clients\n"
    "    -n                 enable network server instead of using shared memory\n"
    "    -x                 remove shared memory file\n"
    "    -g [MAJOR.MINOR]   report specific opengl version (default: %d.%d)\n"
    "    -r [WIDTHxHEIGHT]  set max resolution (default: 1920x1080)\n"
    "    -m [SIZE]          max amount of megabytes program may allocate (default: 16mib)\n"
    "    -p [PORT]          if networking is enabled, specify which port to use (default: 3000)\n";

static void generate_virtual_machine_arguments(size_t m)
{
    static const char *libvirt_string = 
        "<shmem name=\"" SGL_SHARED_MEMORY_NAME "\">\n"
        "  <model type=\"ivshmem-plain\"/>\n"
        "  <size unit=\"M\">%ld</size>\n"
        "</shmem>\n";
    
    static const char *qemu_string =
        "-object memory-backend-file,size=%ldM,share,mem-path=/dev/shm/" SGL_SHARED_MEMORY_NAME ",id=" SGL_SHARED_MEMORY_NAME "\n";

    fprintf(stderr, "\nlibvirt:\n");
    fprintf(stderr, libvirt_string, m);
    fprintf(stderr, "\nqemu:\n");
    fprintf(stderr, qemu_string, m);
    fprintf(stderr, "\n");
}

static void term_handler(int sig)
{
    munmap(shm_ptr, shm_size);
    int icmd = *internal_cmd_ptr;

    switch (sig) {
    case SIGINT:
        break;
    case SIGSEGV:
        PRINT_LOG("server stopped! segmentation fault on %s (%d)\n", sgl_cmd2str(icmd), icmd);
        break;
    // shouldn't ever reach here
    case SIGPIPE:
        PRINT_LOG("server stopped! socket unexpectedly closed\n");
        break;
    }

    exit(1);
}

static void arg_parser_protector(int sig)
{
    PRINT_LOG("expected second argument\n");
    exit(1);
}

int main(int argc, char **argv)
{
    bool print_virtual_machine_arguments = false;

    bool network_over_shared = false;
    int port = 3000;

    int major = SGL_DEFAULT_MAJOR;
    int minor = SGL_DEFAULT_MINOR;

    shm_size = 16;

    signal(SIGSEGV, arg_parser_protector);

    for (int i = 1; i < argc; i++) {
        switch (argv[i][1]) {
        case 'h':
            printf(usage, SGL_DEFAULT_MAJOR, SGL_DEFAULT_MINOR);
            return 0;
        case 'v':
            print_virtual_machine_arguments = true;
            break;
        case 'n':
            network_over_shared = true;
            break;
        case 'o':
            overlay_enable();
            break;
        case 'x':
            shm_unlink(SGL_SHARED_MEMORY_NAME);
            PRINT_LOG("unlinked shared memory '%s'\n", SGL_SHARED_MEMORY_NAME);
            return 0;
        case 'g':
            major = argv[i + 1][0] - '0';
            minor = argv[i + 1][2] - '0';
            i++;
            break;
        case 'r': {
            int width, height, j;
            for (j = 0; j < strlen(argv[i + 1]); j++)
                if (argv[i + 1][j] == 'x')
                    break;
            sgl_set_max_resolution(
                atoi(argv[i + 1]),
                atoi(&argv[i + 1][j + 1])
            );
            i++;
            break;
        }
        case 'm':
            shm_size = atoi(argv[i + 1]);
            i++;
            break;
        case 'p':
            port = atoi(argv[i + 1]);
            i++;
            break;
        default:
            PRINT_LOG("unrecognized command-line option '%s'\n", argv[i]);
        }
    }

    signal(SIGINT, term_handler);
    signal(SIGSEGV, term_handler);
    signal(SIGPIPE, SIG_IGN);

    PRINT_LOG("press CTRL+C to terminate server\n");

    if (print_virtual_machine_arguments) {
        if (!network_over_shared)
            generate_virtual_machine_arguments(shm_size);
        else
            PRINT_LOG("command line argument '-v' ignored as networking is enabled\n");
    }

    /*
     * allocate memory, only create a shared memory file if using shared memory
     */
    shm_size *= 1024 * 1024;
    if (!network_over_shared) {
        int shm_fd = shm_open(SGL_SHARED_MEMORY_NAME, O_CREAT | O_RDWR, S_IRWXU);
        if (shm_fd == -1) {
            PRINT_LOG("failed to open shared memory '%s'\n", SGL_SHARED_MEMORY_NAME);
            return -1;
        }

        if (ftruncate(shm_fd, shm_size) == -1) {
            PRINT_LOG("failed to truncate shared memory\n");
            return -2;
        }

        shm_ptr = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    }
    else {
        shm_ptr = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
    }

    PRINT_LOG("reserved %ld MiB of memory\n", shm_size / 1024 / 1024);

    struct sgl_cmd_processor_args args = {
        .base_address = shm_ptr,
        .memory_size = shm_size,

        .network_over_shared = network_over_shared,
        .port = port,

        .gl_major = major,
        .gl_minor = minor,

        .internal_cmd_ptr = &internal_cmd_ptr,
    };

    int mw, mh;
    sgl_get_max_resolution(&mw, &mh);
    PRINT_LOG("maximum resolution set to %dx%d\n", mw, mh);
    PRINT_LOG("reporting gl version %d.%d\n", major, minor);

    sgl_cmd_processor_start(args);
}