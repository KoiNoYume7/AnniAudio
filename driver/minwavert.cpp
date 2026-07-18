#include "minwavert.h"
#include "minwavertstream.h"

static const ULONG TIMER_PERIOD_MS    = 10;
static const ULONG BUFFER_DURATION_MS = 200;   // 200ms cyclic buffer

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
NTSTATUS CreateMiniportWaveRT(
    _Out_ PUNKNOWN*   ppUnknown,
    _In_opt_ PUNKNOWN pUnknownOuter,
    _In_  POOL_TYPE   poolType)
{
    CMiniportWaveRT* p = new(poolType, ANNI_TAG) CMiniportWaveRT(pUnknownOuter);
    if (!p) return STATUS_INSUFFICIENT_RESOURCES;
    p->AddRef();
    *ppUnknown = static_cast<IMiniportWaveRT*>(p);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// COM identity
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::NonDelegatingQueryInterface(REFIID iid, PVOID* ppv)
{
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IMiniport) ||
        IsEqualGUID(iid, IID_IMiniportWaveRT)) {
        *ppv = static_cast<IMiniportWaveRT*>(this);
        AddRef();
        return STATUS_SUCCESS;
    }
    *ppv = nullptr;
    return STATUS_NOINTERFACE;
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
CMiniportWaveRT::~CMiniportWaveRT()
{
    if (m_TimerInitialized) {
        KeCancelTimer(&m_Timer);
        m_TimerInitialized = FALSE;
    }
    if (m_Port) { m_Port->Release(); m_Port = nullptr; }

    if (m_SharedMdl) {
        IoFreeMdl(m_SharedMdl);
        m_SharedMdl = nullptr;
    }
    if (m_SharedBuffer) {
        ExFreePoolWithTag(m_SharedBuffer, ANNI_TAG);
        m_SharedBuffer = nullptr;
    }
}

// ---------------------------------------------------------------------------
// IMiniport::GetDescription
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::GetDescription(PPCFILTER_DESCRIPTOR* ppDesc)
{
    *ppDesc = &g_WaveRTFilterDescriptor;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// IMiniport::DataRangeIntersection
// Let PortCls do the heavy lifting with its default data-intersection handler.
// We only need to validate that the request is an audio/WAVEFORMATEX range and
// that it can accommodate stereo; if so, return STATUS_NOT_IMPLEMENTED so the
// class driver picks a concrete format from our declared KSDATARANGE_AUDIO list.
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::DataRangeIntersection(
    ULONG PinId, PKSDATARANGE DataRange, PKSDATARANGE MatchingDataRange,
    ULONG OutputBufferLength, PVOID ResultantFormat, PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);

    if (!DataRange || !MatchingDataRange || !ResultantFormatLength) {
        return STATUS_INVALID_PARAMETER;
    }

    // Only accept audio data ranges with a WAVEFORMATEX specifier
    if (!IsEqualGUID(DataRange->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUID(MatchingDataRange->MajorFormat, KSDATAFORMAT_TYPE_AUDIO)) {
        return STATUS_NO_MATCH;
    }

    if (!IsEqualGUID(DataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) ||
        !IsEqualGUID(MatchingDataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
        return STATUS_NO_MATCH;
    }

    // Require at least two channels (stereo) on both sides
    auto* reqAudio   = reinterpret_cast<const KSDATARANGE_AUDIO*>(DataRange);
    auto* matchAudio = reinterpret_cast<const KSDATARANGE_AUDIO*>(MatchingDataRange);
    if (reqAudio->MaximumChannels < 2 || matchAudio->MaximumChannels < 2) {
        return STATUS_NO_MATCH;
    }

    return STATUS_NOT_IMPLEMENTED;
}

// ---------------------------------------------------------------------------
// IMiniportWaveRT::Init
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::Init(
    PUNKNOWN UnknownAdapter, PRESOURCELIST ResourceList, PPORTWAVERT Port)
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);

    m_Port          = Port;
    m_SampleRate    = 48000;
    m_BytesPerFrame = 8; // 2ch float32

    Port->AddRef();

    // Initialize high-resolution timing for the position counter
    KeQueryPerformanceFrequency(&m_QPCFrequency);
    KeQueryPerformanceCounter(&m_LastDpcTime);

    // Initialize timer DPC that simulates hardware position counter
    KeInitializeDpc(&m_Dpc, TimerDpc, this);
    KeInitializeTimer(&m_Timer);
    m_TimerInitialized = TRUE;

    // Fire every TIMER_PERIOD_MS (QPC will be used to compute the real advance)
    LARGE_INTEGER due;
    due.QuadPart = -((LONGLONG)TIMER_PERIOD_MS * 10000); // 100-ns units, negative = relative
    KeSetTimerEx(&m_Timer, due, TIMER_PERIOD_MS, &m_Dpc);

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// IMiniportWaveRT::NewStream
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::NewStream(
    PMINIPORTWAVERTSTREAM* ppStream,
    PPORTWAVERTSTREAM      PortStream,
    ULONG                  Pin,
    BOOLEAN                Capture,
    PKSDATAFORMAT          DataFormat)
{
    UNREFERENCED_PARAMETER(PortStream);

    CMiniportWaveRTStream* pStream =
        new(NonPagedPoolNx, ANNI_TAG) CMiniportWaveRTStream(nullptr);
    if (!pStream) return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS status = pStream->Init(this, Pin, Capture, DataFormat);
    if (!NT_SUCCESS(status)) { delete pStream; return status; }

    pStream->AddRef();
    *ppStream = static_cast<IMiniportWaveRTStream*>(pStream);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// IMiniportWaveRT::GetDeviceDescription
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::GetDeviceDescription(PDEVICE_DESCRIPTION DevDesc)
{
    RtlZeroMemory(DevDesc, sizeof(*DevDesc));
    DevDesc->Version      = DEVICE_DESCRIPTION_VERSION;
    DevDesc->Master       = TRUE;
    DevDesc->ScatterGather = TRUE;
    DevDesc->MaximumLength = MAXULONG;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Timer DPC — advances per-instance position counter
// ---------------------------------------------------------------------------
void NTAPI CMiniportWaveRT::TimerDpc(PKDPC, PVOID Context, PVOID, PVOID)
{
    auto* self = static_cast<CMiniportWaveRT*>(Context);

    LARGE_INTEGER now;
    KeQueryPerformanceCounter(&now);

    KIRQL oldIrql;
    KeAcquireSpinLock(&self->m_PositionLock, &oldIrql);

    LONGLONG delta = now.QuadPart - self->m_LastDpcTime.QuadPart;
    if (delta < 0) delta = 0;

    // advance = elapsed_seconds * sampleRate * bytesPerFrame
    LONGLONG advance = 0;
    if (self->m_QPCFrequency.QuadPart > 0) {
        advance = delta * self->m_SampleRate * self->m_BytesPerFrame
                  / self->m_QPCFrequency.QuadPart;
    }

    self->m_BytesTransferred += advance;
    self->m_LastDpcTime = now;

    KeReleaseSpinLock(&self->m_PositionLock, oldIrql);
}
