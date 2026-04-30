#pragma once

// Provide our own operator new/delete — prevents deprecated inline versions in stdunk.h
#define _NEW_DELETE_OPERATORS_

extern "C" {
#include <ntddk.h>
}

#include <portcls.h>
#include <stdunk.h>
#include <ksdebug.h>

#define ANNI_TAG  'iNNA'   // pool allocation tag
#define ANNI_MAXOBJECTS 32 // max subobjects per device

// ---------------------------------------------------------------------------
// Placement operator new / delete for kernel-mode COM objects.
// Definitions live in adapter.cpp.
// ---------------------------------------------------------------------------
void* __cdecl operator new(size_t size, POOL_TYPE pool, ULONG tag);
void* __cdecl operator new(size_t size, POOL_TYPE pool);
void  __cdecl operator delete(void* p, ULONG tag);
void  __cdecl operator delete(void* p, size_t sz);
void  __cdecl operator delete(void* p);
void  __cdecl operator delete[](void* p);

// Forward declarations
class CMiniportWaveRT;
class CMiniportWaveRTStream;
class CMiniportTopology;

// Factory functions (defined in each .cpp)
NTSTATUS CreateMiniportWaveRT(   _Out_ PUNKNOWN*, _In_opt_ PUNKNOWN, _In_ POOL_TYPE);
NTSTATUS CreateMiniportTopology( _Out_ PUNKNOWN*, _In_opt_ PUNKNOWN, _In_ POOL_TYPE);
