/*
 * this pertains to memory detection within Linux VMs
 */

#include <client/memory.h>

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>

int sgl_detect_device_memory(const char *path)
{
    DIR *directory;
    struct dirent *entry = NULL;

    directory = opendir(path);
    if (directory == NULL)
        return -1;

    while ((entry = readdir(directory)) != NULL) {
        char full_name[256] = { 0 };
        snprintf(full_name, 100, "%s/%s", path, entry->d_name);

        if (entry->d_type == DT_DIR) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                int fd = sgl_detect_device_memory(full_name);
                if (fd != -1)
                    return fd;
            }
        } else {
            if (strstr(full_name, "bar2") != NULL)
                return open(full_name, O_RDWR, S_IRWXU);
        }
    }

    closedir(directory);
    return -1;
}