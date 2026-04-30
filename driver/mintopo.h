#pragma once
#include "common.h"
#include "toptable.h"

// -------------------------------------------------------------------------
// CMiniportTopology
// Minimal topology miniport — just returns the empty filter descriptor so
// PortCls bookkeeping is satisfied.
// -------------------------------------------------------------------------
class CMiniportTopology
    : public IMiniportTopology
    , public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportTopology);
    ~CMiniportTopology() {}

    // IMiniport
    STDMETHODIMP_(NTSTATUS) GetDescription(_Out_ PPCFILTER_DESCRIPTOR* Description);
    STDMETHODIMP_(NTSTATUS) DataRangeIntersection(
        _In_      ULONG        PinId,
        _In_      PKSDATARANGE DataRange,
        _In_      PKSDATARANGE MatchingDataRange,
        _In_      ULONG        OutputBufferLength,
        _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
                  PVOID        ResultantFormat,
        _Out_     PULONG       ResultantFormatLength);

    // IMiniportTopology
    STDMETHODIMP_(NTSTATUS) Init(
        _In_ PUNKNOWN         UnknownAdapter,
        _In_ PRESOURCELIST    ResourceList,
        _In_ PPORTTOPOLOGY    Port);
};
