#include "minwavertstream.h"
#include "minwavert.h"

// ---------------------------------------------------------------------------
// COM identity
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::NonDelegatingQueryInterface(REFIID iid, PVOID* ppv)
{
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IMiniportWaveRTStream)) {
        *ppv = static_cast<IMiniportWaveRTStream*>(this);
        AddRef();
        return STATUS_SUCCESS;
    }
    *ppv = nullptr;
    return STATUS_NOINTERFACE;
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
CMiniportWaveRTStream::~CMiniportWaveRTStream()
{
    // The shared MDL/buffer is owned by the miniport; we do not free it here.
    m_Mdl = nullptr;
}

// ---------------------------------------------------------------------------
// Init — called right after construction
// ---------------------------------------------------------------------------
NTSTATUS CMiniportWaveRTStream::Init(
    CMiniportWaveRT* Miniport,
    ULONG            Pin,
    BOOLEAN          Capture,
    PKSDATAFORMAT    DataFormat)
{
    m_pMiniport = Miniport;
    m_Pin       = Pin;
    m_Capture   = Capture;
    m_State     = KSSTATE_STOP;
    m_Mdl       = nullptr;

    // Extract sample rate from the format if it's WAVEFORMATEX-based
    m_SampleRate    = 48000;
    m_BytesPerFrame = 8;
    m_BufferSize    = 0;

    if (DataFormat && DataFormat->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
        auto* wfxFmt = reinterpret_cast<KSDATAFORMAT_WAVEFORMATEX*>(DataFormat);
        m_SampleRate    = wfxFmt->WaveFormatEx.nSamplesPerSec;
        m_BytesPerFrame = wfxFmt->WaveFormatEx.nBlockAlign;
        // Update miniport state to match negotiated format (TimerDpc reads these)
        KIRQL oldIrql;
        KeAcquireSpinLock(&Miniport->m_PositionLock, &oldIrql);
        Miniport->m_SampleRate    = m_SampleRate;
        Miniport->m_BytesPerFrame = m_BytesPerFrame;
        KeReleaseSpinLock(&Miniport->m_PositionLock, oldIrql);
    }

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// IMiniportWaveRTStream::SetFormat
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::SetFormat(PKSDATAFORMAT DataFormat)
{
    if (!DataFormat) return STATUS_INVALID_PARAMETER;
    if (DataFormat->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
        auto* wfxFmt = reinterpret_cast<KSDATAFORMAT_WAVEFORMATEX*>(DataFormat);
        m_SampleRate    = wfxFmt->WaveFormatEx.nSamplesPerSec;
        m_BytesPerFrame = wfxFmt->WaveFormatEx.nBlockAlign;

        KIRQL oldIrql;
        KeAcquireSpinLock(&m_pMiniport->m_PositionLock, &oldIrql);
        m_pMiniport->m_SampleRate    = m_SampleRate;
        m_pMiniport->m_BytesPerFrame = m_BytesPerFrame;
        KeReleaseSpinLock(&m_pMiniport->m_PositionLock, oldIrql);
    }
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// IMiniportWaveRTStream::SetState
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::SetState(KSSTATE State)
{
    m_State = State;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// IMiniportWaveRTStream::GetPosition
// Reports the current DMA position using the timer-driven counter.
// 
// Position semantics:
//   Render:  PlayOffset = hardware read position (where driver reads from buffer)
//            WriteOffset = where app can write to (ahead of PlayOffset)
//   Capture: WriteOffset = hardware write position (where driver writes to buffer)
//            PlayOffset = where app can read from (behind WriteOffset)
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::GetPosition(PKSAUDIO_POSITION Position)
{
    if (m_BufferSize == 0 || m_State == KSSTATE_STOP) {
        Position->PlayOffset  = 0;
        Position->WriteOffset = 0;
        return STATUS_SUCCESS;
    }

    // Read position counter with lock held (sync with TimerDpc)
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_pMiniport->m_PositionLock, &oldIrql);
    ULONG64 pos = (ULONG64)(m_pMiniport->m_BytesTransferred) % m_BufferSize;
    KeReleaseSpinLock(&m_pMiniport->m_PositionLock, oldIrql);

    if (m_Capture) {
        // Capture: driver writes at pos; app reads the valid data behind it.
        // WriteOffset = next write position (current hardware write pointer).
        // PlayOffset  = oldest valid sample, one frame after WriteOffset in the cycle
        // so the whole ring except the next write slot is exposed to the app.
        Position->WriteOffset = pos;
        Position->PlayOffset  = (pos + m_BytesPerFrame) % m_BufferSize;
    } else {
        // Render: driver reads at pos; app writes ahead of it.
        // PlayOffset  = current hardware read pointer.
        // WriteOffset = just before PlayOffset in the cycle, exposing the
        // entire ring except the sample currently being consumed.
        Position->PlayOffset  = pos;
        Position->WriteOffset = (pos + m_BufferSize - m_BytesPerFrame) % m_BufferSize;
    }
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// IMiniportWaveRTStream::AllocateAudioBuffer
// Both render and capture share one physical buffer owned by the miniport.
// The first call allocates; subsequent calls map the same MDL.
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::AllocateAudioBuffer(
    ULONG RequestedSize, PMDL* AudioBufferMdl,
    ULONG* ActualSize, ULONG* OffsetFromFirstPage,
    MEMORY_CACHING_TYPE* CacheType)
{
    // Round up to page boundary
    ULONG size = (RequestedSize + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (!m_pMiniport->m_SharedBuffer) {
        // First allocation for this cable — create the shared buffer
        m_pMiniport->m_SharedBuffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, size, ANNI_TAG);
        if (!m_pMiniport->m_SharedBuffer) return STATUS_INSUFFICIENT_RESOURCES;

        RtlZeroMemory(m_pMiniport->m_SharedBuffer, size);
        m_pMiniport->m_SharedBufferSize = size;

        m_pMiniport->m_SharedMdl = IoAllocateMdl(
            m_pMiniport->m_SharedBuffer, size, FALSE, FALSE, nullptr);
        if (!m_pMiniport->m_SharedMdl) {
            ExFreePoolWithTag(m_pMiniport->m_SharedBuffer, ANNI_TAG);
            m_pMiniport->m_SharedBuffer = nullptr;
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        MmBuildMdlForNonPagedPool(m_pMiniport->m_SharedMdl);
    }

    *AudioBufferMdl      = m_pMiniport->m_SharedMdl;
    *ActualSize          = m_pMiniport->m_SharedBufferSize;
    *OffsetFromFirstPage = 0;
    *CacheType           = MmCached;
    m_Mdl                = m_pMiniport->m_SharedMdl;
    m_BufferSize         = m_pMiniport->m_SharedBufferSize;

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// IMiniportWaveRTStream::FreeAudioBuffer
// We do NOT free the shared MDL here (the miniport owns it).
// ---------------------------------------------------------------------------
STDMETHODIMP_(VOID) CMiniportWaveRTStream::FreeAudioBuffer(PMDL AudioBufferMdl, ULONG BufferSize)
{
    UNREFERENCED_PARAMETER(AudioBufferMdl);
    UNREFERENCED_PARAMETER(BufferSize);
    m_Mdl        = nullptr;
    m_BufferSize = 0;
}

// ---------------------------------------------------------------------------
// IMiniportWaveRTStream::GetHWLatency
// ---------------------------------------------------------------------------
STDMETHODIMP_(VOID) CMiniportWaveRTStream::GetHWLatency(KSRTAUDIO_HWLATENCY* hwLatency)
{
    hwLatency->FifoSize      = m_BytesPerFrame;
    hwLatency->ChipsetDelay  = 0;
    hwLatency->CodecDelay    = 0;
}

// ---------------------------------------------------------------------------
// IMiniportWaveRTStream::GetPositionRegister
// We have no hardware register — return STATUS_NOT_IMPLEMENTED so the OS
// uses GetPosition() polling instead.
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::GetPositionRegister(KSRTAUDIO_HWREGISTER* Register)
{
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_IMPLEMENTED;
}

// ---------------------------------------------------------------------------
// IMiniportWaveRTStream::GetClockRegister
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::GetClockRegister(KSRTAUDIO_HWREGISTER* Register)
{
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_IMPLEMENTED;
}
