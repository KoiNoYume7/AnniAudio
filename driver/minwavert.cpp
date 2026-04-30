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
// Validate that the requested format matches one of our supported ranges.
// We only accept IEEE-float / stereo / 48k or 96k.
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::DataRangeIntersection(
    ULONG PinId, PKSDATARANGE DataRange, PKSDATARANGE MatchingDataRange,
    ULONG OutputBufferLength, PVOID ResultantFormat, PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(PinId);

    if (!DataRange || !MatchingDataRange || !ResultantFormatLength) {
        return STATUS_INVALID_PARAMETER;
    }

    // Only accept audio data ranges
    if (!IsEqualGUID(DataRange->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUID(MatchingDataRange->MajorFormat, KSDATAFORMAT_TYPE_AUDIO)) {
        return STATUS_NO_MATCH;
    }

    // Both must specify WAVEFORMATEX specifier
    if (!IsEqualGUID(DataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) ||
        !IsEqualGUID(MatchingDataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
        return STATUS_NO_MATCH;
    }

    // Cast to audio data range to inspect channel/sample-rate constraints
    auto* reqAudio = reinterpret_cast<const KSDATARANGE_AUDIO*>(DataRange);
    auto* matchAudio = reinterpret_cast<const KSDATARANGE_AUDIO*>(MatchingDataRange);

    // Must support IEEE_FLOAT subtype (allow wildcard KSDATAFORMAT_SUBTYPE_WILDCARD)
    bool subOk = IsEqualGUID(DataRange->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ||
                 IsEqualGUID(MatchingDataRange->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ||
                 IsEqualGUID(DataRange->SubFormat, KSDATAFORMAT_SUBTYPE_WILDCARD) ||
                 IsEqualGUID(MatchingDataRange->SubFormat, KSDATAFORMAT_SUBTYPE_WILDCARD);
    if (!subOk) return STATUS_NO_MATCH;

    // Pick a supported rate (prefer 48000, fallback 96000 if the request wants it)
    ULONG chosenRate = 48000;
    if (reqAudio->MaximumSampleFrequency >= 96000 &&
        matchAudio->MaximumSampleFrequency >= 96000) {
        chosenRate = 96000;
    }

    // Chosen rate must be inside both ranges
    if (chosenRate < reqAudio->MinimumSampleFrequency || chosenRate > reqAudio->MaximumSampleFrequency ||
        chosenRate < matchAudio->MinimumSampleFrequency || chosenRate > matchAudio->MaximumSampleFrequency) {
        return STATUS_NO_MATCH;
    }

    // Channels: require exactly 2 (stereo)
    if (reqAudio->MaximumChannels < 2 || matchAudio->MaximumChannels < 2) {
        return STATUS_NO_MATCH;
    }

    ULONG required = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    *ResultantFormatLength = required;
    if (OutputBufferLength == 0) return STATUS_BUFFER_OVERFLOW;
    if (OutputBufferLength < required) return STATUS_BUFFER_TOO_SMALL;
    if (!ResultantFormat) return STATUS_INVALID_PARAMETER;

    auto* out = static_cast<KSDATAFORMAT_WAVEFORMATEXTENSIBLE*>(ResultantFormat);
    RtlZeroMemory(out, required);

    out->DataFormat.FormatSize  = required;
    out->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    out->DataFormat.SubFormat   = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    out->DataFormat.Specifier   = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    out->DataFormat.SampleSize  = 8; // 2ch * 4 bytes

    WAVEFORMATEXTENSIBLE& wfx = out->WaveFormatExt;
    wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels       = 2;
    wfx.Format.nSamplesPerSec  = chosenRate;
    wfx.Format.wBitsPerSample  = 32;
    wfx.Format.nBlockAlign     = 8;
    wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * 8;
    wfx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask           = KSAUDIO_SPEAKER_STEREO;
    wfx.SubFormat               = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    return STATUS_SUCCESS;
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

    // Initialize timer DPC that simulates hardware position counter
    KeInitializeDpc(&m_Dpc, TimerDpc, this);
    KeInitializeTimer(&m_Timer);
    m_TimerInitialized = TRUE;

    // Fire every TIMER_PERIOD_MS
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
    LONG64 advance = (LONG64)(self->m_SampleRate) * TIMER_PERIOD_MS / 1000
                   * self->m_BytesPerFrame;
    InterlockedAdd64(&self->m_BytesTransferred, advance);
}
