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

static const char *usage =
    "usage: sharedgl [-h] [-v] [-o] [-x] [-g MAJOR.MINOR] [-r WIDTHxHEIGHT] [-m SIZE]\n"
    "\n"
    "options:\n"
    "    -h                 display help information\n"
    "    -v                 display virtual machine arguments\n"
    "    -o                 enables fps overlay on clients (shows server side fps)\n"
    "    -x                 remove shared memory file\n"
    "    -g [MAJOR.MINOR]   report specific opengl version (default: 1.2)\n"
    "    -r [WIDTHxHEIGHT]  set max resolution (default: 1920x1080)\n"
    "    -m [SIZE]          max amount of megabytes program may allocate (default: 16mib)\n";

static void generate_virtual_machine_arguments(size_t m)
{
    static const char *libvirt_string = 
        "<shmem name=\"" SGL_SHARED_MEMORY_NAME "\">\n"
        "  <model type=\"ivshmem-plain\"/>\n"
        "  <size unit=\"M\">%ld</size>\n"
        "</shmem>\n";
    
    static const char *qemu_string =
        "-object memory-backend-file,size=%ldM,share,mem-path=/dev/shm/" SGL_SHARED_MEMORY_NAME ",id=" SGL_SHARED_MEMORY_NAME "\n";

    printf("[*] libvirt:\n");
    printf(libvirt_string, m);
    printf("\n[*] qemu:\n");
    printf(qemu_string, m);
    printf("\n");
}

static void term_handler(int a)
{
    munmap(shm_ptr, shm_size);
    // shm_unlink(SGL_SHARED_MEMORY_NAME);

    puts("");
    exit(1);
}

int main(int argc, char **argv)
{
    shm_size = 16;
    bool print_virtual_machine_arguments = false;

    int major = 1;
    int minor = 2;

    for (int i = 1; i < argc; i++) {
        switch (argv[i][1]) {
        case 'h':
            printf("%s", usage);
            return 0;
        case 'v':
            print_virtual_machine_arguments = true;
            break;
        case 'o':
            overlay_enable();
            break;
        case 'x':
            shm_unlink(SGL_SHARED_MEMORY_NAME);
            return 0;
        case 'g':
            major = atoi(argv[i + 1]);
            minor = atoi(&argv[i + 1][2]);
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
        default:
            printf("[?] unknown argument '%s'\n", argv[i]);
        }
    }

    printf("[*] press CTRL+C to terminate server\n\n");
    signal(SIGINT, term_handler);

    printf("[*] reporting gl version %d.%d\n\n", major, minor);

    if (print_virtual_machine_arguments)
        generate_virtual_machine_arguments(shm_size);
    printf("[*] using %ld MiB of memory\n", shm_size);
    shm_size *= 1024 * 1024;

    int shm_fd = shm_open(SGL_SHARED_MEMORY_NAME, O_CREAT | O_RDWR, S_IRWXU);
    if (shm_fd == -1) {
        fprintf(stderr, "[-] failed to open shared memory '%s'\n", SGL_SHARED_MEMORY_NAME);
        return -1;
    }

    if (ftruncate(shm_fd, shm_size) == -1) {
        fprintf(stderr, "[-] failed to truncate shared memory\n");
        return -1;
    }

    shm_ptr = mmap(0, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    sgl_cmd_processor_start(shm_size, shm_ptr, major, minor);
}