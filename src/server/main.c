#define SHAREDGL_HOST
#include <sharedgl.h>
#include <server/processor.h>
#include <server/overlay.h>
#include <server/context.h>

#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

#include <stdio.h>
#include <stdlib.h>

static void *shm_ptr;
static size_t shm_size;

int *internal_cmd_ptr;

static const char *usage =
    "usage: sglrenderer [-h] [-v] [-o] [-n] [-x] [-g MAJOR.MINOR] [-r WIDTHxHEIGHT] [-m SIZE] [-p PORT]\n"
    "\n"
    "options:\n"
    "    -h                 display help information\n"
    "    -v                 display virtual machine arguments\n"
    "    -o                 enables fps overlay on clients (shows server side fps)\n"
    "    -n                 enable network server instead of using shared memory\n"
    "    -x                 remove shared memory file\n"
    "    -g [MAJOR.MINOR]   report specific opengl version (default: 2.1)\n"
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

    printf("%slibvirt%s:\n", COLOR_UNIQ, COLOR(COLOR_RESET));
    printf(libvirt_string, m);
    printf("\n%sqemu%s:\n", COLOR_UNIQ, COLOR(COLOR_RESET));
    printf(qemu_string, m);
    printf("\n");
}

static void term_handler(int sig)
{
    munmap(shm_ptr, shm_size);

    switch (sig) {
    case SIGINT:
        break;
    case SIGSEGV:
        printf("%sfatal%s: server stopped: segmentation fault (cmd: %s%d%s)", COLOR_ERRO, COLOR(COLOR_RESET), COLOR_NUMB, *internal_cmd_ptr, COLOR(COLOR_RESET));
        break;
    }

    puts("");
    exit(1);
}

static void arg_parser_protector(int sig)
{
    printf("%sfatal%s: expected second argument\n", COLOR_ERRO, COLOR(COLOR_RESET));
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
            printf("%s", usage);
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
            printf("%serr%s: unknown argument \"%s\"\n", COLOR_ERRO, COLOR(COLOR_RESET), argv[i]);
        }
    }

    signal(SIGINT, term_handler);
    signal(SIGSEGV, term_handler);

    printf("%sinfo%s: press %sCTRL+C%s to terminate server\n\n", COLOR_INFO, COLOR(COLOR_RESET), COLOR_NUMB, COLOR(COLOR_RESET));
    printf("%sinfo%s: reporting gl version %s%d%s.%s%d%s\n\n", COLOR_INFO, COLOR(COLOR_RESET), COLOR_NUMB, major, COLOR(COLOR_RESET), COLOR_NUMB, minor, COLOR(COLOR_RESET));

    if (print_virtual_machine_arguments) {
        if (!network_over_shared)
            generate_virtual_machine_arguments(shm_size);
        else
            printf("%sinfo%s: command line argument '-v' ignored as networking is enabled\n\n", COLOR_INFO, COLOR(COLOR_RESET));
    }

    printf("%sinfo%s: using %s%ld%s MiB of memory\n", COLOR_INFO, COLOR(COLOR_RESET), COLOR_NUMB, shm_size, COLOR(COLOR_RESET));

    /*
     * allocate memory, only create a shared memory file if using shared memory
     */
    shm_size *= 1024 * 1024;
    if (!network_over_shared) {
        int shm_fd = shm_open(SGL_SHARED_MEMORY_NAME, O_CREAT | O_RDWR, S_IRWXU);
        if (shm_fd == -1) {
            printf("%serr%s: failed to open shared memory '%s'\n", COLOR_ERRO, COLOR(COLOR_RESET), SGL_SHARED_MEMORY_NAME);
            return -1;
        }

        if (ftruncate(shm_fd, shm_size) == -1) {
            printf("%serr%s: failed to truncate shared memory\n", COLOR_ERRO, COLOR(COLOR_RESET));
            return -2;
        }

        shm_ptr = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    }
    else {
        shm_ptr = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
    }

    struct sgl_cmd_processor_args args = {
        .base_address = shm_ptr,
        .memory_size = shm_size,

        .network_over_shared = network_over_shared,
        .port = port,

        .gl_major = major,
        .gl_minor = minor,

        .internal_cmd_ptr = &internal_cmd_ptr,
    };

    sgl_cmd_processor_start(args);
}