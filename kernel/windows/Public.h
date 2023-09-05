#include <initguid.h>

DEFINE_GUID (GUID_DEVINTERFACE_KSGLDRV,
    0xdf576976,0x569d,0x4672,0x95,0xa0,0xf5,0x7e,0x4e,0xa0,0xb2,0x10);
// {df576976-569d-4672-95a0-f57e4ea0b210}

typedef UINT16 KSGLDRV_PEERID;
typedef UINT64 KSGLDRV_SIZE;

#define KSGLDRV_CACHE_NONCACHED     0
#define KSGLDRV_CACHE_CACHED        1
#define KSGLDRV_CACHE_WRITECOMBINED 2

/*
    This structure is for use with the IOCTL_KSGLDRV_REQUEST_MMAP IOCTL
*/
typedef struct KSGLDRV_MMAP_CONFIG
{
    UINT8 cacheMode; // the caching mode of the mapping, see KSGLDRV_CACHE_* for options
}
KSGLDRV_MMAP_CONFIG, *PKSGLDRV_MMAP_CONFIG;

/*
    This structure is for use with the IOCTL_KSGLDRV_REQUEST_MMAP IOCTL
*/
typedef struct KSGLDRV_MMAP
{
    KSGLDRV_PEERID peerID;  // our peer id
    KSGLDRV_SIZE   size;    // the size of the memory region
    PVOID          ptr;     // pointer to the memory region
    UINT16         vectors; // the number of vectors available
}
KSGLDRV_MMAP, *PKSGLDRV_MMAP;

/*
    This structure is for use with the IOCTL_KSGLDRV_RING_DOORBELL IOCTL
*/
typedef struct KSGLDRV_RING
{
    KSGLDRV_PEERID peerID;  // the id of the peer to ring
    UINT16         vector;  // the doorbell to ring
}
KSGLDRV_RING, *PKSGLDRV_RING;

/*
   This structure is for use with the IOCTL_KSGLDRV_REGISTER_EVENT IOCTL

   Please Note:
     - The KSGLDRV driver has a hard limit of 32 events.
     - Events that are singleShot are released after they have been set.
     - At this time repeating events are only released when the driver device
       handle is closed, closing the event handle doesn't release it from the
       drivers list. While this won't cause a problem in the driver, it will
       cause you to run out of event slots.
 */
typedef struct KSGLDRV_EVENT
{
    UINT16  vector;     // the vector that triggers the event
    HANDLE  event;      // the event to trigger
    BOOLEAN singleShot; // set to TRUE if you want the driver to only trigger this event once
}
KSGLDRV_EVENT, *PKSGLDRV_EVENT;

#define IOCTL_KSGLDRV_REQUEST_PEERID CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KSGLDRV_REQUEST_SIZE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KSGLDRV_REQUEST_MMAP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KSGLDRV_RELEASE_MMAP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KSGLDRV_RING_DOORBELL  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KSGLDRV_REGISTER_EVENT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KSGLDRV_REQUEST_KMAP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
