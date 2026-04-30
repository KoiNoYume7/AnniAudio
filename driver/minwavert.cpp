#include "minwavert.h"
#include "minwavertstream.h"

// ---------------------------------------------------------------------------
// Shared cyclic buffer (render writes, capture reads)
// ---------------------------------------------------------------------------
PVOID          g_SharedBuffer      = nullptr;
PMDL           g_SharedMdl         = nullptr;
ULONG          g_SharedBufferSize  = 0;
volatile LONG64 g_BytesTransferred = 0;

static const ULONG TIMER_PERIOD_MS   = 10;
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
    KeCancelTimer(&m_Timer);
    if (m_Port) { m_Port->Release(); m_Port = nullptr; }

    if (g_SharedMdl) {
        IoFreeMdl(g_SharedMdl);
        g_SharedMdl = nullptr;
    }
    if (g_SharedBuffer) {
        ExFreePoolWithTag(g_SharedBuffer, ANNI_TAG);
        g_SharedBuffer = nullptr;
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
// We accept exact-match requests for our supported IEEE float formats.
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::DataRangeIntersection(
    ULONG PinId, PKSDATARANGE DataRange, PKSDATARANGE MatchingDataRange,
    ULONG OutputBufferLength, PVOID ResultantFormat, PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);

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
    wfx.Format.nSamplesPerSec  = m_SampleRate ? m_SampleRate : 48000;
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
// Timer DPC — advances global position counter
// ---------------------------------------------------------------------------
void NTAPI CMiniportWaveRT::TimerDpc(PKDPC, PVOID Context, PVOID, PVOID)
{
    auto* self = static_cast<CMiniportWaveRT*>(Context);
    LONG64 advance = (LONG64)(self->m_SampleRate) * TIMER_PERIOD_MS / 1000
                   * self->m_BytesPerFrame;
    InterlockedAdd64(&g_BytesTransferred, advance);
}
