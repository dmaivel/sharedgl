#include <client/scratch.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#define __USE_GNU
#define _GNU_SOURCE
#include <unistd.h>
#include <linux/mman.h>
#include <sys/mman.h>
#endif

static void *address = NULL;
static size_t current_size = 0;

static inline uintptr_t align_to_4kb(uintptr_t ptr)
{
    return (ptr + 4095) & ~4095;
}

#ifdef _WIN32
static void *windows_mremap(void *old_address, size_t old_size, size_t new_size)
{
    /*
     * check to see if we can extend the current allocation
     */
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((char*)old_address + old_size, &mbi, sizeof(mbi)) == sizeof(mbi))
        if (mbi.State == MEM_FREE && mbi.RegionSize >= (new_size - old_size))
            if (VirtualAlloc((char*)old_address + old_size, new_size - old_size, MEM_COMMIT, PAGE_READWRITE))
                return old_address;
    
    /*
     * otherwise, allocate a new region, copy, free
     * this can be expensive, but will rarely be performed
     */
    void *new_address = VirtualAlloc(NULL, new_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (new_address) {
        memcpy(new_address, old_address, old_size);
        VirtualFree(old_address, 0, MEM_RELEASE);
        return new_address;
    }
    
    return NULL;
}
#endif

void *scratch_buffer_get(size_t size)
{
    if (!current_size) {
        current_size = align_to_4kb(size);
#ifdef _WIN32
        address = VirtualAlloc(NULL, current_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
        address = mmap(NULL, current_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
        return address;
    }
    else {
        size_t aligned_size = align_to_4kb(size);
        if (current_size < aligned_size) {
#ifdef _WIN32
            address = windows_mremap(address, current_size, aligned_size);
#else
            address = mremap(address, current_size, aligned_size, MREMAP_MAYMOVE);
#endif
            current_size = aligned_size;
            return address;
        }
        
        return address;
    }
}