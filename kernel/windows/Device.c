#include "driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, KSGLDRVCreateDevice)
#pragma alloc_text (PAGE, KSGLDRVEvtDevicePrepareHardware)
#pragma alloc_text (PAGE, KSGLDRVEvtD0Exit)
#endif

NTSTATUS KSGLDRVCreateDevice(_Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    PDEVICE_CONTEXT deviceContext;
    WDFDEVICE device;
    NTSTATUS status;

    PAGED_CODE();
    DEBUG_INFO("%s", __FUNCTION__);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = KSGLDRVEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = KSGLDRVEvtDeviceReleaseHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry         = KSGLDRVEvtD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit          = KSGLDRVEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, NULL, NULL, KSGLDRVEvtDeviceFileCleanup);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);

    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("%s", "Call to WdfDeviceCreate failed");
        return status;
    }

    deviceContext = DeviceGetContext(device);
    RtlZeroMemory(deviceContext, sizeof(DEVICE_CONTEXT));
    KeInitializeSpinLock(&deviceContext->eventListLock);
    InitializeListHead(&deviceContext->eventList);

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_KSGLDRV, NULL);

    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("%s", "Call to WdfDeviceCreateDeviceInterface failed");
        return status;
    }

    status = KSGLDRVQueueInitialize(device);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("%s", "KSGLDRVQueueInitialize failed");
        return status;
    }

    return status;
}

PVOID KSGLDRVMmMapIoSpace(
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ SIZE_T NumberOfBytes
    )
{
    typedef
    PVOID
    (*PFN_MM_MAP_IO_SPACE_EX) (
        _In_ PHYSICAL_ADDRESS PhysicalAddress,
        _In_ SIZE_T NumberOfBytes,
        _In_ ULONG Protect
        );

    UNICODE_STRING         name;
    PFN_MM_MAP_IO_SPACE_EX pMmMapIoSpaceEx;

    RtlInitUnicodeString(&name, L"MmMapIoSpaceEx");
    pMmMapIoSpaceEx = (PFN_MM_MAP_IO_SPACE_EX) (ULONG_PTR)MmGetSystemRoutineAddress(&name);

    if (pMmMapIoSpaceEx != NULL){
        // Call WIN10 API if available
        return pMmMapIoSpaceEx(PhysicalAddress,
                               NumberOfBytes,
                               PAGE_READWRITE | PAGE_NOCACHE);
    }

    #pragma warning(suppress: 30029)
    return MmMapIoSpace(PhysicalAddress, NumberOfBytes, MmNonCached);
}


NTSTATUS KSGLDRVEvtDevicePrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesRaw, _In_ WDFCMRESLIST ResourcesTranslated)
{
    PAGED_CODE();
    DEBUG_INFO("%s", __FUNCTION__);
    PDEVICE_CONTEXT deviceContext;
    deviceContext = DeviceGetContext(Device);

#if (NTDDI_VERSION < NTDDI_WIN8)
    UNREFERENCED_PARAMETER(ResourcesRaw);
#endif
    NTSTATUS result = STATUS_SUCCESS;
    int memIndex = 0;

    const ULONG resCount = WdfCmResourceListGetCount(ResourcesTranslated);
    for (ULONG i = 0; i < resCount; ++i)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor;
        descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (!descriptor)
        {
            DEBUG_ERROR("%s", "Call to WdfCmResourceListGetDescriptor failed");
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        if (descriptor->Type == CmResourceTypeInterrupt && descriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE)
            ++deviceContext->interruptCount;
    }

      if (deviceContext->interruptCount > 0)
      {
          deviceContext->interrupts = (WDFINTERRUPT*)ExAllocatePoolUninitialized(KSGLDRV_NONPAGED_POOL,
              sizeof(WDFINTERRUPT) * deviceContext->interruptCount, 'sQRI');

          if (!deviceContext->interrupts)
          {
              DEBUG_ERROR("Failed to allocate space for %d interrupts", deviceContext->interrupts);
              return STATUS_INSUFFICIENT_RESOURCES;
          }
      }

    for (ULONG i = 0; i < resCount; ++i)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor;
        descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (!descriptor)
        {
            DEBUG_ERROR("%s", "Call to WdfCmResourceListGetDescriptor failed");
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        if (descriptor->Type == CmResourceTypeMemory)
        {
            // control registers
            if (memIndex == 0)
            {
                if (descriptor->u.Memory.Length != sizeof(KSGLDRVDeviceRegisters))
                {
                    DEBUG_ERROR("Resource size was %u long when %u was expected",
                        descriptor->u.Memory.Length, sizeof(KSGLDRVDeviceRegisters));
                    result = STATUS_DEVICE_HARDWARE_ERROR;
                    break;
                }

                deviceContext->devRegisters = (PKSGLDRVDeviceRegisters)KSGLDRVMmMapIoSpace(
                    descriptor->u.Memory.Start,
                    descriptor->u.Memory.Length);

                if (!deviceContext->devRegisters)
                {
                    DEBUG_ERROR("%s", "Call to MmMapIoSpace failed");
                    result = STATUS_DEVICE_HARDWARE_ERROR;
                    break;
                }
            }
            else
            // shared memory resource
            if ((deviceContext->interruptCount == 0 && memIndex == 1) || memIndex == 2)
            {
                deviceContext->shmemAddr.PhysicalAddress = descriptor->u.Memory.Start;
                deviceContext->shmemAddr.NumberOfBytes = descriptor->u.Memory.Length;
                DEBUG_INFO("memIndex = %d pa = %llx (%llx) size = %lx (%lx)", memIndex, descriptor->u.Memory.Start.QuadPart, deviceContext->shmemAddr.PhysicalAddress.QuadPart, descriptor->u.Memory.Length, deviceContext->shmemAddr.NumberOfBytes);

#if (NTDDI_VERSION >= NTDDI_WIN8)
                result = MmAllocateMdlForIoSpace(&deviceContext->shmemAddr, 1, &deviceContext->shmemMDL);
#else
                deviceContext->shmemAddr.VirtualAddress = MmMapIoSpace(deviceContext->shmemAddr.PhysicalAddress, deviceContext->shmemAddr.NumberOfBytes, MmNonCached);
                if (deviceContext->shmemAddr.VirtualAddress) {
                    deviceContext->shmemMDL = IoAllocateMdl(deviceContext->shmemAddr.VirtualAddress, (ULONG)deviceContext->shmemAddr.NumberOfBytes, FALSE, FALSE, NULL);
                    if (!deviceContext->shmemMDL) {
                        DEBUG_INFO("%s", "Call to IoAllocateMdl failed");
                        result = STATUS_INSUFFICIENT_RESOURCES;
                    }

                    MmBuildMdlForNonPagedPool(deviceContext->shmemMDL);
                }
                else {
                    DEBUG_INFO("%s", "Call to MmMapIoSpace failed");
                    result = STATUS_INSUFFICIENT_RESOURCES;
                }
#endif
                if (!NT_SUCCESS(result))
                {
                    DEBUG_ERROR("%s", "Call to MmAllocateMdlForIoSpace failed");
                    break;
                }
            }
            DEBUG_INFO("memIndex = %d va = %p mdl = %p", memIndex, deviceContext->shmemAddr.VirtualAddress, deviceContext->shmemMDL);
            ++memIndex;
            continue;
        }

        if (descriptor->Type == CmResourceTypeInterrupt &&
            (descriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE))
        {
            WDF_INTERRUPT_CONFIG irqConfig;
            WDF_INTERRUPT_CONFIG_INIT(&irqConfig,
                KSGLDRVInterruptISR,
                KSGLDRVInterruptDPC);
#if (NTDDI_VERSION >= NTDDI_WIN8)
            irqConfig.InterruptTranslated = descriptor;
            irqConfig.InterruptRaw = WdfCmResourceListGetDescriptor(ResourcesRaw, i);
#endif
            NTSTATUS status = WdfInterruptCreate(Device, &irqConfig, WDF_NO_OBJECT_ATTRIBUTES,
                &deviceContext->interrupts[deviceContext->interruptsUsed]);

            if (!NT_SUCCESS(status))
            {
                DEBUG_ERROR("Call to WdfInterruptCreate failed: %08x", status);
                result = status;
                break;
            }

            if (++deviceContext->interruptsUsed == 65)
              DEBUG_INFO("%s", "This driver does not support > 64 interrupts, they will be ignored in the ISR.");

            continue;
        }
    }

    if (NT_SUCCESS(result))
    {
        if (!deviceContext->shmemMDL) {
            DEBUG_ERROR("%s", "shmemMDL == NULL");
            result = STATUS_DEVICE_HARDWARE_ERROR;
        }
        else
        {
            DEBUG_INFO("Shared Memory: %llx, %lx bytes", deviceContext->shmemAddr.PhysicalAddress.QuadPart, deviceContext->shmemAddr.NumberOfBytes);
            DEBUG_INFO("Interrupts   : %d", deviceContext->interruptsUsed);
        }
    }
    DEBUG_INFO("%s result 0x%x", __FUNCTION__, result);
    return result;
}

NTSTATUS KSGLDRVEvtDeviceReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    DEBUG_INFO("%s", __FUNCTION__);

    PDEVICE_CONTEXT deviceContext;
    deviceContext = DeviceGetContext(Device);

    if (deviceContext->devRegisters)
    {
        MmUnmapIoSpace(deviceContext->devRegisters, sizeof(PKSGLDRVDeviceRegisters));
    }

    if (deviceContext->shmemMap)
    {
        MmUnmapLockedPages(deviceContext->shmemMap, deviceContext->shmemMDL);
        deviceContext->shmemMap = NULL;
    }

    if (deviceContext->shmemMDL)
    {
        IoFreeMdl(deviceContext->shmemMDL);
        deviceContext->shmemMDL = NULL;
    }

    if (deviceContext->interrupts)
    {
        // WDFINTERRUPT objects are deleted by the framework

        ExFreePoolWithTag(deviceContext->interrupts, 'sQRI');

        deviceContext->interruptCount = 0;
        deviceContext->interruptsUsed = 0;
        deviceContext->interrupts = NULL;
    }

    KIRQL oldIRQL;
    KeAcquireSpinLock(&deviceContext->eventListLock, &oldIRQL);
    PLIST_ENTRY entry = deviceContext->eventList.Flink;
    while (entry != &deviceContext->eventList)
    {
        _Analysis_assume_(entry != NULL);
        PKSGLDRVEventListEntry event = CONTAINING_RECORD(entry, KSGLDRVEventListEntry, ListEntry);
        if (event->event)
        {
            ObDereferenceObject(event->event);
        }
        event->owner  = NULL;
        event->event  = NULL;
        event->vector = 0;

        entry = entry->Flink;
    }
    InitializeListHead(&deviceContext->eventList);
    deviceContext->eventBufferUsed = 0;
    KeReleaseSpinLock(&deviceContext->eventListLock, oldIRQL);

    return STATUS_SUCCESS;
}

NTSTATUS KSGLDRVEvtD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    DEBUG_INFO("%s", __FUNCTION__);
    return STATUS_SUCCESS;
}

NTSTATUS KSGLDRVEvtD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    PAGED_CODE();
    DEBUG_INFO("%s", __FUNCTION__);
    return STATUS_SUCCESS;
}

BOOLEAN KSGLDRVInterruptISR(_In_ WDFINTERRUPT Interrupt, _In_ ULONG MessageID)
{
    WDFDEVICE device;
    PDEVICE_CONTEXT deviceContext;

    // out of range. if you have this many you're doing it wrong anyway
    if (MessageID > 64)
        return TRUE;

    device = WdfInterruptGetDevice(Interrupt);
    deviceContext = DeviceGetContext(device);

    if (!InterlockedOr64(&deviceContext->pendingISR, 1ULL << MessageID))
        WdfInterruptQueueDpcForIsr(Interrupt);

    return TRUE;
}

void KSGLDRVInterruptDPC(_In_ WDFINTERRUPT Interrupt, _In_ WDFOBJECT AssociatedObject)
{
    UNREFERENCED_PARAMETER(AssociatedObject);

    WDFDEVICE device;
    PDEVICE_CONTEXT deviceContext;
    UINT64 pending;

    device = WdfInterruptGetDevice(Interrupt);
    deviceContext = DeviceGetContext(device);
    
    pending = InterlockedExchange64(&deviceContext->pendingISR, 0);
    if (!pending)
        return;

    KeAcquireSpinLockAtDpcLevel(&deviceContext->eventListLock);
    PLIST_ENTRY entry = deviceContext->eventList.Flink;
    while (entry != &deviceContext->eventList)
    {
        PKSGLDRVEventListEntry event = CONTAINING_RECORD(entry, KSGLDRVEventListEntry, ListEntry);
        PLIST_ENTRY next = entry->Flink;
        if (pending & ((LONG64)1 << event->vector))
        {
            _Analysis_assume_(event->event != NULL);
            KeSetEvent(event->event, 0, FALSE);
            if (event->singleShot)
            {
                RemoveEntryList(entry);
                ObDereferenceObjectDeferDelete(event->event);
                event->owner  = NULL;
                event->event  = NULL;
                event->vector = 0;
                --deviceContext->eventBufferUsed;
            }
        }
        entry = next;
    }
    KeReleaseSpinLockFromDpcLevel(&deviceContext->eventListLock);
}
