#pragma once
#include "common.h"
#include "toptable.h"

// -------------------------------------------------------------------------
// CMiniportWaveRTStream
// Implements IMiniportWaveRTStream.
// Both render and capture streams share the same cyclic buffer that lives
// in the miniport (g_sharedBuffer / g_sharedMdl).
// -------------------------------------------------------------------------
class CMiniportWaveRTStream
    : public IMiniportWaveRTStream
    , public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveRTStream);
    ~CMiniportWaveRTStream();

    // IMiniportWaveRTStream
    STDMETHODIMP_(NTSTATUS) SetFormat(_In_ PKSDATAFORMAT DataFormat);
    STDMETHODIMP_(NTSTATUS) SetState(_In_ KSSTATE State);
    STDMETHODIMP_(NTSTATUS) GetPosition(_Out_ PKSAUDIO_POSITION Position);
    STDMETHODIMP_(NTSTATUS) AllocateAudioBuffer(
        _In_  ULONG  RequestedSize,
        _Out_ PMDL*  AudioBufferMdl,
        _Out_ ULONG* ActualSize,
        _Out_ ULONG* OffsetFromFirstPage,
        _Out_ MEMORY_CACHING_TYPE* CacheType);
    STDMETHODIMP_(VOID)     FreeAudioBuffer(_In_opt_ PMDL AudioBufferMdl, _In_ ULONG BufferSize);
    STDMETHODIMP_(VOID)     GetHWLatency(_Out_ KSRTAUDIO_HWLATENCY* hwLatency);
    STDMETHODIMP_(NTSTATUS) GetPositionRegister(_Out_ KSRTAUDIO_HWREGISTER* Register);
    STDMETHODIMP_(NTSTATUS) GetClockRegister(_Out_ KSRTAUDIO_HWREGISTER* Register);

    // Called by the miniport to wire up context
    NTSTATUS Init(
        _In_ CMiniportWaveRT* Miniport,
        _In_ ULONG            Pin,
        _In_ BOOLEAN          Capture,
        _In_ PKSDATAFORMAT    DataFormat);

private:
    CMiniportWaveRT* m_pMiniport;
    ULONG            m_Pin;
    BOOLEAN          m_Capture;
    KSSTATE          m_State;
    ULONG            m_SampleRate;
    ULONG            m_BytesPerFrame; // channels * bytes-per-sample
    ULONG            m_BufferSize;
    PMDL             m_Mdl;           // MDL for this stream's view of the buffer
};
