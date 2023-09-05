#include "public.h"

EXTERN_C_START

#define MAX_EVENTS 32

#pragma align(push,4)
typedef struct KSGLDRVDeviceRegisters
{
    volatile ULONG irqMask;
    volatile ULONG irqStatus;
    volatile LONG  ivProvision;
    volatile ULONG doorbell;
    volatile UCHAR reserved[240];
}
KSGLDRVDeviceRegisters, *PKSGLDRVDeviceRegisters;
#pragma align(pop)

typedef struct KSGLDRVEventListEntry
{
    WDFFILEOBJECT owner;
    UINT16        vector;
    PRKEVENT      event;
    BOOLEAN       singleShot;
    LIST_ENTRY    ListEntry;
}
KSGLDRVEventListEntry, *PKSGLDRVEventListEntry;

#if (NTDDI_VERSION < NTDDI_WIN8)
typedef struct _MM_PHYSICAL_ADDRESS_LIST {
    PHYSICAL_ADDRESS PhysicalAddress;
    PVOID            VirtualAddress;
    SIZE_T           NumberOfBytes;
} MM_PHYSICAL_ADDRESS_LIST, *PMM_PHYSICAL_ADDRESS_LIST;
#endif

typedef struct _DEVICE_CONTEXT
{
    PKSGLDRVDeviceRegisters devRegisters; // the device registers (BAR0)

    MM_PHYSICAL_ADDRESS_LIST   shmemAddr;               // physical address of the shared memory (BAR2)
    PMDL                       shmemMDL;                // memory discriptor list of the shared memory
    PVOID                      shmemMap;                // memory mapping of the shared memory
    WDFFILEOBJECT              owner;                   // the file object that currently owns the mapping
    UINT16                     interruptCount;          // the number of interrupt entries allocated
    UINT16                     interruptsUsed;          // the number of interrupt entries used
    WDFINTERRUPT              *interrupts;              // interrupts for this device
    LONG64                     pendingISR;              // flags for ISRs pending processing

    KSPIN_LOCK                 eventListLock;           // spinlock for the below event list
    KSGLDRVEventListEntry      eventBuffer[MAX_EVENTS]; // buffer of pre-allocated events
    UINT16                     eventBufferUsed;         // number of events currenty in use
    LIST_ENTRY                 eventList;               // pending events to fire
}
DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

NTSTATUS KSGLDRVCreateDevice(_Inout_ PWDFDEVICE_INIT DeviceInit);

EVT_WDF_DEVICE_PREPARE_HARDWARE KSGLDRVEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE KSGLDRVEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY KSGLDRVEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT KSGLDRVEvtD0Exit;
EVT_WDF_INTERRUPT_ISR KSGLDRVInterruptISR;
EVT_WDF_INTERRUPT_DPC KSGLDRVInterruptDPC;

EXTERN_C_END
