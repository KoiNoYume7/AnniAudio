#include "mintopo.h"

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
NTSTATUS CreateMiniportTopology(
    _Out_ PUNKNOWN*   ppUnknown,
    _In_opt_ PUNKNOWN pUnknownOuter,
    _In_  POOL_TYPE   poolType)
{
    CMiniportTopology* p = new(poolType, ANNI_TAG) CMiniportTopology(pUnknownOuter);
    if (!p) return STATUS_INSUFFICIENT_RESOURCES;
    p->AddRef();
    *ppUnknown = static_cast<IMiniportTopology*>(p);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// COM identity
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportTopology::NonDelegatingQueryInterface(REFIID iid, PVOID* ppv)
{
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IMiniport) ||
        IsEqualGUID(iid, IID_IMiniportTopology)) {
        *ppv = static_cast<IMiniportTopology*>(this);
        AddRef();
        return STATUS_SUCCESS;
    }
    *ppv = nullptr;
    return STATUS_NOINTERFACE;
}

// ---------------------------------------------------------------------------
// IMiniport::GetDescription
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportTopology::GetDescription(PPCFILTER_DESCRIPTOR* ppDesc)
{
    *ppDesc = &g_TopoFilterDescriptor;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// IMiniport::DataRangeIntersection — topology pins carry no audio data
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportTopology::DataRangeIntersection(
    ULONG, PKSDATARANGE, PKSDATARANGE, ULONG, PVOID, PULONG ResultantFormatLength)
{
    *ResultantFormatLength = 0;
    return STATUS_NOT_IMPLEMENTED;
}

// ---------------------------------------------------------------------------
// IMiniportTopology::Init
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportTopology::Init(
    PUNKNOWN UnknownAdapter, PRESOURCELIST ResourceList, PPORTTOPOLOGY Port)
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port);
    return STATUS_SUCCESS;
}
