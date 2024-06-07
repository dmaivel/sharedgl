#include <sharedgl.h>
#include <client/pb.h>
#include <stdio.h>
#include <string.h>

#include <inttypes.h>

#ifndef _WIN32
#define __USE_GNU
#define _GNU_SOURCE
#include <sys/mman.h>
#else
#include <windows.h>
#include <SetupAPI.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#include <initguid.h>

#pragma comment (lib, "Setupapi.lib")

DEFINE_GUID(GUID_DEVINTERFACE_IVSHMEM,
    0xdf576976, 0x569d, 0x4672, 0x95, 0xa0, 0xf5, 0x7e, 0x4e, 0xa0, 0xb2, 0x10);

#define IVSHMEM_CACHE_NONCACHED     0
#define IVSHMEM_CACHE_CACHED        1
#define IVSHMEM_CACHE_WRITECOMBINED 2

typedef struct IVSHMEM_MMAP_CONFIG {
    BYTE CacheMode;
} IVSHMEM_MMAP_CONFIG, *PIVSHMEM_MMAP_CONFIG;

typedef struct IVSHMEM_MMAP {
    WORD    PeerID;
    DWORD64 Size;
    PVOID   Pointer;
    WORD    Vectors;
} IVSHMEM_MMAP, *PIVSHMEM_MMAP;

#define IOCTL_IVSHMEM_REQUEST_SIZE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_REQUEST_MMAP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_RELEASE_MMAP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

HANDLE Handle;
#endif

void *ptr;
void *base;
int *cur;

void *in_base;
int *in_cur;

struct pb_net_hooks net_hooks = { NULL };

#ifndef _WIN32
void pb_set(int fd)
{
    ptr = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    
    int alloc_size = *(int*)(ptr + SGL_OFFSET_REGISTER_MEMSIZE);
    munmap(ptr, 0x1000);
    ptr = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    base = ptr + 0x1000;

    in_base = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    in_cur = in_base;
}
#else
void pb_set(void)
{
    HDEVINFO DeviceInfoSet;
    PSP_DEVICE_INTERFACE_DETAIL_DATA InfData;
    SP_DEVICE_INTERFACE_DATA DeviceInterfaceData;
    DWORD64 Size;
    IVSHMEM_MMAP_CONFIG Config;
    IVSHMEM_MMAP Map;
    DWORD RequestSize;

    DeviceInfoSet = SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES | DIGCF_DEVICEINTERFACE);
    ZeroMemory(&DeviceInterfaceData, sizeof(SP_DEVICE_INTERFACE_DATA));
    DeviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    if (SetupDiEnumDeviceInterfaces(DeviceInfoSet, NULL, &GUID_DEVINTERFACE_IVSHMEM, 0, &DeviceInterfaceData) == FALSE)
        return;

    SetupDiGetDeviceInterfaceDetail(DeviceInfoSet, &DeviceInterfaceData, NULL, 0, &RequestSize, NULL);
    if (!RequestSize)
        return;

    InfData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)(malloc(RequestSize));
    ZeroMemory(InfData, RequestSize);
    InfData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
    if (!SetupDiGetDeviceInterfaceDetail(DeviceInfoSet, &DeviceInterfaceData, InfData, RequestSize, NULL, NULL))
        return;
    
    Handle = CreateFile(InfData->DevicePath, 0, 0, NULL, OPEN_EXISTING, 0, 0);
    if (Handle == INVALID_HANDLE_VALUE)
        return;

    if (InfData)
        free(InfData);
    SetupDiDestroyDeviceInfoList(DeviceInfoSet);

    if (!DeviceIoControl(Handle, IOCTL_IVSHMEM_REQUEST_SIZE, NULL, 0, &Size, sizeof(UINT64), NULL, NULL))
        return;

    Config.CacheMode = IVSHMEM_CACHE_WRITECOMBINED;
    ZeroMemory(&Map, sizeof(IVSHMEM_MMAP));
    if (!DeviceIoControl(Handle, IOCTL_IVSHMEM_REQUEST_MMAP, &Config, sizeof(IVSHMEM_MMAP_CONFIG), &Map, sizeof(IVSHMEM_MMAP), NULL, NULL))
        return;

    ptr = Map.Pointer;
    base = (PVOID)((DWORD64)Map.Pointer + (DWORD64)0x1000);

    in_base = VirtualAlloc(NULL, Map.Size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    in_cur = in_base;
}

void pb_unset(void)
{
    DeviceIoControl(Handle, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL, 0, NULL, NULL);
    CloseHandle(Handle);
}
#endif

void pb_set_net(struct pb_net_hooks hooks, size_t internal_alloc_size)
{
    net_hooks = hooks;

#ifndef _WIN32
    in_base = mmap(NULL, internal_alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    in_cur = in_base;
#else
    in_base = VirtualAlloc(NULL, internal_alloc_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    in_cur = in_base;
#endif
}

void pb_reset()
{
    cur = base;
    in_cur = in_base;
    // pb_ctx.current_offset = pb_ctx.reset_offset;
}

void pb_push(int c)
{
    *in_cur++ = c;
    // pb_ctx.current_offset += sizeof(c);
}

void pb_pushf(float c)
{
    *in_cur++ = *(int*)&c;
    // pb_ctx.current_offset += sizeof(c);
}

int pb_read(int s)
{
    if (net_hooks._pb_read)
        return net_hooks._pb_read(s);
    return *(int*)((size_t)ptr + s);
}

int64_t pb_read64(int s)
{
    if (net_hooks._pb_read64)
        return net_hooks._pb_read64(s);
    return *(int64_t*)((size_t)ptr + s);
}

void pb_write(int s, int c)
{
    *(int*)((size_t)ptr + s) = c;
}

void pb_copy(void *data, int s, size_t length)
{
    memcpy(data, (void*)((size_t)ptr + s), length);
}

/*
 *  // equivalent to
 *  int *pdata = (int*)data;
 *  for (int i = 0; i < length / 4; i++)
 *      pb_push(*pdata++);
 */
void pb_memcpy(void *src, size_t length)
{
    // length = length - (length % 4);
    memcpy(in_cur, src, length);
    in_cur += (length / 4);
}

void *pb_ptr(size_t offs)
{
    if (net_hooks._pb_ptr)
        return net_hooks._pb_ptr(offs);
    return (void*)((size_t)ptr + offs);
}

void *pb_iptr(size_t offs)
{
    return (void*)((size_t)in_base + offs);
}

size_t pb_size()
{
    return (size_t)in_cur - (size_t)in_base;
}

void pb_copy_to_shared()
{
    memcpy(base, in_base, (size_t)in_cur - (size_t)in_base);
}