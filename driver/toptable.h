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

// 48 kHz / stereo / 16-bit PCM
static KSDATARANGE_AUDIO g_RangePcm16_48kHz = {
    {
        sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
        { STATIC_KSDATAFORMAT_TYPE_AUDIO },
        { STATIC_KSDATAFORMAT_SUBTYPE_PCM },
        { STATIC_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX }
    },
    2, 16, 16, 48000, 48000
};

// 96 kHz / stereo / 16-bit PCM
static KSDATARANGE_AUDIO g_RangePcm16_96kHz = {
    {
        sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
        { STATIC_KSDATAFORMAT_TYPE_AUDIO },
        { STATIC_KSDATAFORMAT_SUBTYPE_PCM },
        { STATIC_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX }
    },
    2, 16, 16, 96000, 96000
};

// ---- Wave host pins (sink/source opened by apps) ----
static const PKSDATARANGE g_WaveHostRanges[] = {
    (PKSDATARANGE)&g_Range48kHz,
    (PKSDATARANGE)&g_Range96kHz,
    (PKSDATARANGE)&g_RangePcm16_48kHz,
    (PKSDATARANGE)&g_RangePcm16_96kHz,
};

// ---- Wave bridge pins (physically connected to the topology filter) ----
static const PKSDATARANGE g_WaveBridgeRanges[] = {
    (PKSDATARANGE)&g_Range48kHz,
    (PKSDATARANGE)&g_Range96kHz,
    (PKSDATARANGE)&g_RangePcm16_48kHz,
    (PKSDATARANGE)&g_RangePcm16_96kHz,
};

// -------------------------------------------------------------------------
// WaveRT pin descriptors
//
// Pin 0: render host sink (IN)      -- app writes here
// Pin 1: render bridge source (OUT) -- physically connected to topology input
// Pin 2: capture bridge sink (IN)   -- physically connected from topology output
// Pin 3: capture host source (OUT)  -- app reads here
// -------------------------------------------------------------------------
static PCPIN_DESCRIPTOR g_WaveRTPins[] = {
    {   // Pin 0: render host sink
        ULONG(-1), ULONG(-1), 0, nullptr,
        {
            0, nullptr,
            0, nullptr,
            ARRAYSIZE(g_WaveHostRanges), g_WaveHostRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            nullptr,
            0
        }
    },
    {   // Pin 1: render bridge source (to topology)
        ULONG(-1), ULONG(-1), 0, nullptr,
        {
            0, nullptr,
            0, nullptr,
            ARRAYSIZE(g_WaveBridgeRanges), g_WaveBridgeRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            nullptr,
            0
        }
    },
    {   // Pin 2: capture bridge sink (from topology)
        ULONG(-1), ULONG(-1), 0, nullptr,
        {
            0, nullptr,
            0, nullptr,
            ARRAYSIZE(g_WaveBridgeRanges), g_WaveBridgeRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            nullptr,
            0
        }
    },
    {   // Pin 3: capture host streaming pin
        ULONG(-1), ULONG(-1), 0, nullptr,
        {
            0, nullptr,
            0, nullptr,
            ARRAYSIZE(g_WaveHostRanges), g_WaveHostRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            nullptr,
            0
        }
    }
};

// Internal connections: host sink -> bridge source for each direction
static PCCONNECTION_DESCRIPTOR g_WaveRTConnections[] = {
    { PCFILTER_NODE, 0, PCFILTER_NODE, 1 }, // render sink  -> render source
    { PCFILTER_NODE, 2, PCFILTER_NODE, 3 }, // capture sink -> capture source
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
    ARRAYSIZE(g_WaveRTConnections), // ConnectionCount
    g_WaveRTConnections,            // Connections
    ARRAYSIZE(g_WaveRTCategories),  // CategoryCount
    g_WaveRTCategories              // Categories
};

// -------------------------------------------------------------------------
// Topology miniport bridge pins
// AudioEndpointBuilder creates one WASAPI endpoint per bridge pin:
//   KSPIN_DATAFLOW_OUT bridge  -> RENDER endpoint  (speaker)
//   KSPIN_DATAFLOW_IN  bridge  -> CAPTURE endpoint (microphone)
// The internal connections route from the physically connected side to the
// bridge side; PcRegisterPhysicalConnection (adapter.cpp) wires each side
// to the matching WaveRT bridge pin.
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

// Topology pins:
//  0: render input (IN)     -- physically connected from WaveRT render source
//  1: speaker output (OUT)  -- RENDER endpoint
//  2: microphone input (IN) -- CAPTURE endpoint
//  3: capture output (OUT)  -- physically connected to WaveRT capture sink
static PCPIN_DESCRIPTOR g_TopoPins[] = {
    {   // Pin 0: render input from WaveRT
        1, 1, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_TopoBridgeRanges), g_TopoBridgeRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_ANALOG_CONNECTOR,
            nullptr,
            0
        }
    },
    {   // Pin 1: speaker -- render endpoint
        1, 1, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_TopoBridgeRanges), g_TopoBridgeRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_SPEAKER,
            nullptr,
            0
        }
    },
    {   // Pin 2: microphone -- capture endpoint
        1, 1, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_TopoBridgeRanges), g_TopoBridgeRanges,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_MICROPHONE,
            nullptr,
            0
        }
    },
    {   // Pin 3: capture output to WaveRT
        1, 1, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_TopoBridgeRanges), g_TopoBridgeRanges,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_ANALOG_CONNECTOR,
            nullptr,
            0
        }
    }
};

// Internal topology: input -> endpoint, endpoint -> output
static PCCONNECTION_DESCRIPTOR g_TopoConnections[] = {
    { PCFILTER_NODE, 0, PCFILTER_NODE, 1 }, // render input -> speaker
    { PCFILTER_NODE, 2, PCFILTER_NODE, 3 }, // microphone  -> capture output
};

static const GUID g_TopoCategories[] = {
    { STATICGUIDOF(KSCATEGORY_TOPOLOGY) },   // required: AudioEndpointBuilder searches this category
};

static PCFILTER_DESCRIPTOR g_TopoFilterDescriptor = {
    0, nullptr,
    sizeof(PCPIN_DESCRIPTOR), ARRAYSIZE(g_TopoPins), g_TopoPins,
    0, 0, nullptr,
    ARRAYSIZE(g_TopoConnections), g_TopoConnections,
    ARRAYSIZE(g_TopoCategories), g_TopoCategories
};
