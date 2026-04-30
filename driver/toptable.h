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
    { STATICGUIDOF(KSCATEGORY_REALTIME) },   // required: identifies this as a WaveRT filter
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
// Topology miniport bridge pins
// AudioEndpointBuilder creates one WASAPI endpoint per bridge pin:
//   KSPIN_DATAFLOW_OUT bridge  →  RENDER endpoint  (speaker)
//   KSPIN_DATAFLOW_IN  bridge  →  CAPTURE endpoint (microphone)
// PcRegisterPhysicalConnection (in adapter.cpp) wires these to the WaveRT pins.
//
// Bridge pins require:
//   1. A bridge data range (KSDATAFORMAT_SUBTYPE_ANALOG, no specifier)
//   2. At least one PCCONNECTION_DESCRIPTOR linking the two pins so PortCls
//      can walk the topology graph without returning STATUS_BAD_FUNCTION_TABLE.
// -------------------------------------------------------------------------

// Bridge data range: analog audio, no streaming specifier
static const KSDATARANGE g_BridgeDataRange = {
    sizeof(KSDATARANGE), 0, 0, 0,
    { STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO) },
    { STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG) },
    { STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE) }
};
static const PKSDATARANGE g_TopoBridgeRanges[] = {
    const_cast<PKSDATARANGE>(&g_BridgeDataRange)
};

static PCPIN_DESCRIPTOR g_TopoPins[] = {
    {   // Pin 0: render bridge — AudioEndpointBuilder sees DATAFLOW_OUT → render
        1, 1, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_TopoBridgeRanges), g_TopoBridgeRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_BRIDGE,
            &KSNODETYPE_SPEAKER,
            nullptr,
            0
        }
    },
    {   // Pin 1: capture bridge — AudioEndpointBuilder sees DATAFLOW_IN → capture
        1, 1, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_TopoBridgeRanges), g_TopoBridgeRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_BRIDGE,
            &KSNODETYPE_MICROPHONE,
            nullptr,
            0
        }
    }
};

// Internal connection: render bridge (pin 0) → capture bridge (pin 1)
// PCFILTER_NODE means the endpoint is a filter pin (not an internal node).
static const PCCONNECTION_DESCRIPTOR g_TopoConnections[] = {
    { PCFILTER_NODE, 0, PCFILTER_NODE, 1 }
};

static const GUID g_TopoCategories[] = {
    { STATICGUIDOF(KSCATEGORY_AUDIO) },
    { STATICGUIDOF(KSCATEGORY_TOPOLOGY) },   // required: AudioEndpointBuilder searches this category
};

static PCFILTER_DESCRIPTOR g_TopoFilterDescriptor = {
    0, nullptr,
    sizeof(PCPIN_DESCRIPTOR), ARRAYSIZE(g_TopoPins), g_TopoPins,
    0, 0, nullptr,
    ARRAYSIZE(g_TopoConnections), g_TopoConnections,
    ARRAYSIZE(g_TopoCategories), g_TopoCategories
};
