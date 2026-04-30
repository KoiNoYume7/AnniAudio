#pragma once
#include "common.h"

// -------------------------------------------------------------------------
// Supported data ranges
// -------------------------------------------------------------------------

// 48 kHz / stereo / IEEE float 32-bit
static KSDATARANGE_AUDIO g_Range48kHz = {
    {
        sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
        { STATIC_KSDATAFORMAT_TYPE_AUDIO },
        { STATIC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT },
        { STATIC_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX }
    },
    2,          // MaximumChannels
    32, 32,     // Min/MaxBitsPerSample
    48000, 48000
};

// 96 kHz / stereo / IEEE float 32-bit
static KSDATARANGE_AUDIO g_Range96kHz = {
    {
        sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
        { STATIC_KSDATAFORMAT_TYPE_AUDIO },
        { STATIC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT },
        { STATIC_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX }
    },
    2, 32, 32, 96000, 96000
};

// ---- Render pin ----
static const PKSDATARANGE g_RenderRanges[] = {
    (PKSDATARANGE)&g_Range48kHz,
    (PKSDATARANGE)&g_Range96kHz,
};

// ---- Capture pin ----
static const PKSDATARANGE g_CaptureRanges[] = {
    (PKSDATARANGE)&g_Range48kHz,
    (PKSDATARANGE)&g_Range96kHz,
};

// -------------------------------------------------------------------------
// Pin descriptors  (Pin 0 = render sink, Pin 1 = capture source)
// -------------------------------------------------------------------------
static PCPIN_DESCRIPTOR g_WaveRTPins[] = {
    {   // Pin 0: render — app writes, device "plays"
        ULONG(-1), ULONG(-1), 0, nullptr,   // MaxGlobal, MaxFilter, MinFilter, Automation
        {
            0, nullptr,                      // Interfaces
            0, nullptr,                      // Mediums
            ARRAYSIZE(g_RenderRanges), g_RenderRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSNODETYPE_SPEAKER,
            nullptr,
            0
        }
    },
    {   // Pin 1: capture — device writes, app reads
        ULONG(-1), ULONG(-1), 0, nullptr,
        {
            0, nullptr,
            0, nullptr,
            ARRAYSIZE(g_CaptureRanges), g_CaptureRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SOURCE,
            &KSNODETYPE_MICROPHONE,
            nullptr,
            0
        }
    }
};

// -------------------------------------------------------------------------
// Filter categories for WaveRT subdevice
// -------------------------------------------------------------------------
static const GUID g_WaveRTCategories[] = {
    { STATICGUIDOF(KSCATEGORY_AUDIO)  },
    { STATICGUIDOF(KSCATEGORY_RENDER)  },
    { STATICGUIDOF(KSCATEGORY_CAPTURE) },
};

// -------------------------------------------------------------------------
// PCFILTER_DESCRIPTOR for the WaveRT miniport
// -------------------------------------------------------------------------
static PCFILTER_DESCRIPTOR g_WaveRTFilterDescriptor = {
    0,                              // Version
    nullptr,                        // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),       // PinSize
    ARRAYSIZE(g_WaveRTPins),        // PinCount
    g_WaveRTPins,                   // Pins
    0, 0, nullptr,                  // Nodes (none)
    0, nullptr,                     // Connections (none)
    ARRAYSIZE(g_WaveRTCategories),  // CategoryCount
    g_WaveRTCategories              // Categories
};

// -------------------------------------------------------------------------
// Topology miniport: one bridge-in and one bridge-out
// These just satisfy PortCls bookkeeping; no real nodes/connections needed.
// -------------------------------------------------------------------------
static const GUID g_TopoCategories[] = {
    { STATICGUIDOF(KSCATEGORY_AUDIO) },
};

static PCFILTER_DESCRIPTOR g_TopoFilterDescriptor = {
    0, nullptr,
    sizeof(PCPIN_DESCRIPTOR), 0, nullptr,
    0, 0, nullptr,
    0, nullptr,
    ARRAYSIZE(g_TopoCategories), g_TopoCategories
};
