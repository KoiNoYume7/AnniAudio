#define INITGUID
#include "common.h"
#include "minwavert.h"
#include "minwavertstream.h"
#include "mintopo.h"

// ---------------------------------------------------------------------------
// Operator new / delete for kernel mode (required because we defined
// _NEW_DELETE_OPERATORS_ to suppress the deprecated inline versions in stdunk.h)
// ---------------------------------------------------------------------------
void* __cdecl operator new(size_t sz, POOL_TYPE pool, ULONG tag)
{
    UNREFERENCED_PARAMETER(pool);
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, sz, tag);
}
void* __cdecl operator new(size_t sz, POOL_TYPE pool)
{
    UNREFERENCED_PARAMETER(pool);
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, sz, ANNI_TAG);
}
void __cdecl operator delete(void* p, ULONG)  { if (p) ExFreePoolWithTag(p, ANNI_TAG); }
void __cdecl operator delete(void* p, size_t) { if (p) ExFreePool(p); }
void __cdecl operator delete(void* p)         { if (p) ExFreePool(p); }
void __cdecl operator delete[](void* p)       { if (p) ExFreePoolWithTag(p, ANNI_TAG); }

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
extern "C" DRIVER_ADD_DEVICE AddDevice;
NTSTATUS StartDevice(PDEVICE_OBJECT DeviceObject, PIRP Irp, PRESOURCELIST ResourceList);

// ---------------------------------------------------------------------------
// DriverEntry
// ---------------------------------------------------------------------------
extern "C"
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    return PcInitializeAdapterDriver(DriverObject, RegistryPath, AddDevice);
}

// ---------------------------------------------------------------------------
// AddDevice — called by PnP when the device node is enumerated
// ---------------------------------------------------------------------------
extern "C"
NTSTATUS AddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject)
{
    return PcAddAdapterDevice(DriverObject, PhysicalDeviceObject,
                              StartDevice, ANNI_MAXOBJECTS, 0);
}

// ---------------------------------------------------------------------------
// StartDevice — called after resources are assigned; creates port/miniport pairs
// ---------------------------------------------------------------------------
NTSTATUS StartDevice(PDEVICE_OBJECT DeviceObject, PIRP Irp, PRESOURCELIST ResourceList)
{
    NTSTATUS status     = STATUS_SUCCESS;
    PUNKNOWN miniportUnk = nullptr;
    PPORT    wavePort   = nullptr;
    PPORT    topoPort   = nullptr;

    // ---- 1. WaveRT subdevice ----
    status = CreateMiniportWaveRT(&miniportUnk, nullptr, NonPagedPoolNx);
    if (!NT_SUCCESS(status)) return status;

    status = PcNewPort(&wavePort, CLSID_PortWaveRT);
    if (!NT_SUCCESS(status)) { miniportUnk->Release(); return status; }

    status = wavePort->Init(DeviceObject, Irp, miniportUnk, nullptr, ResourceList);
    miniportUnk->Release(); miniportUnk = nullptr;

    if (NT_SUCCESS(status)) {
        status = PcRegisterSubdevice(DeviceObject, L"AnniWave", wavePort);
    }
    if (!NT_SUCCESS(status)) { wavePort->Release(); return status; }

    // ---- 2. Topology subdevice ----
    status = CreateMiniportTopology(&miniportUnk, nullptr, NonPagedPoolNx);
    if (!NT_SUCCESS(status)) { wavePort->Release(); return status; }

    status = PcNewPort(&topoPort, CLSID_PortTopology);
    if (!NT_SUCCESS(status)) { miniportUnk->Release(); wavePort->Release(); return status; }

    status = topoPort->Init(DeviceObject, Irp, miniportUnk, nullptr, ResourceList);
    miniportUnk->Release(); miniportUnk = nullptr;

    if (NT_SUCCESS(status)) {
        status = PcRegisterSubdevice(DeviceObject, L"AnniTopo", topoPort);
    }
    if (!NT_SUCCESS(status)) { topoPort->Release(); wavePort->Release(); return status; }

    // ---- 3. Physical connections (required for AudioEndpointBuilder) ----
    NTSTATUS connStatus;
    connStatus = PcRegisterPhysicalConnection(DeviceObject, wavePort, 0, topoPort, 0);
    if (NT_SUCCESS(status) && !NT_SUCCESS(connStatus)) status = connStatus;

    connStatus = PcRegisterPhysicalConnection(DeviceObject, topoPort, 1, wavePort, 1);
    if (NT_SUCCESS(status) && !NT_SUCCESS(connStatus)) status = connStatus;

    topoPort->Release();
    wavePort->Release();
    return status;
}
