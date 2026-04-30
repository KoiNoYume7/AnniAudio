#pragma once
#include "common.h"
#include "toptable.h"

// -------------------------------------------------------------------------
// CMiniportWaveRT
// Implements IMiniportWaveRT.
// Each instance owns its own shared cyclic buffer so that multiple
// driver instances (one per virtual cable) are fully isolated.
// -------------------------------------------------------------------------
class CMiniportWaveRT
    : public IMiniportWaveRT
    , public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    CMiniportWaveRT(PUNKNOWN pUnknownOuter)
        : CUnknown(pUnknownOuter)
        , m_Port(nullptr)
        , m_SampleRate(0)
        , m_BytesPerFrame(0)
        , m_SharedBuffer(nullptr)
        , m_SharedMdl(nullptr)
        , m_SharedBufferSize(0)
        , m_BytesTransferred(0)
        , m_TimerInitialized(FALSE)
    {
        RtlZeroMemory(&m_Timer, sizeof(m_Timer));
        RtlZeroMemory(&m_Dpc,  sizeof(m_Dpc));
    }
    ~CMiniportWaveRT();

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

    // IMiniportWaveRT
    STDMETHODIMP_(NTSTATUS) Init(
        _In_ PUNKNOWN      UnknownAdapter,
        _In_ PRESOURCELIST ResourceList,
        _In_ PPORTWAVERT   Port);
    STDMETHODIMP_(NTSTATUS) NewStream(
        _Out_ PMINIPORTWAVERTSTREAM*  Stream,
        _In_  PPORTWAVERTSTREAM       PortStream,
        _In_  ULONG                   Pin,
        _In_  BOOLEAN                 Capture,
        _In_  PKSDATAFORMAT           DataFormat);
    STDMETHODIMP_(NTSTATUS) GetDeviceDescription(
        _Out_ PDEVICE_DESCRIPTION DeviceDescription);

    // Timer DPC — drives the position counter
    static void NTAPI TimerDpc(PKDPC Dpc, PVOID Context, PVOID, PVOID);

    PPORTWAVERT   m_Port;
    KTIMER        m_Timer;
    KDPC          m_Dpc;
    ULONG         m_SampleRate;
    ULONG         m_BytesPerFrame;

    // Per-cable shared buffer (render writes, capture reads)
    PVOID         m_SharedBuffer;
    PMDL          m_SharedMdl;
    ULONG         m_SharedBufferSize;
    volatile LONG64 m_BytesTransferred;

    BOOLEAN       m_TimerInitialized;
};
