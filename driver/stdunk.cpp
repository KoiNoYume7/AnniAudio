// stdunk.cpp
// Implements CUnknown — the base COM object for kernel-mode PortCls drivers.
// portcls.lib only exports the PortCls/KS factory/port functions; the driver
// is responsible for compiling CUnknown itself.

#include "common.h"

// ---------------------------------------------------------------------------
// CUnknown::CUnknown
// Sets initial ref count to 0.  The factory calls AddRef() right after
// construction, bringing it to 1 before the pointer is returned.
// When pUnknownOuter is null (non-aggregated), m_pUnknownOuter is set to
// the object's own INonDelegatingUnknown vtable so that GetOuterUnknown()->
// AddRef/Release dispatch through the non-delegating path.
// ---------------------------------------------------------------------------
CUnknown::CUnknown(PUNKNOWN pUnknownOuter)
    : m_lRefCount(0)
{
    if (pUnknownOuter) {
        m_pUnknownOuter = pUnknownOuter;
    } else {
        // Safe cast: INonDelegatingUnknown and IUnknown share the same
        // physical vtable layout (3 slots), so routing through this pointer
        // dispatches QueryInterface→NonDelegatingQueryInterface, etc.
        m_pUnknownOuter = reinterpret_cast<PUNKNOWN>(
                              static_cast<INonDelegatingUnknown*>(this));
    }
}

// ---------------------------------------------------------------------------
// CUnknown::~CUnknown
// ---------------------------------------------------------------------------
CUnknown::~CUnknown()
{
    // Nothing — derived destructors release their own resources.
}

// ---------------------------------------------------------------------------
// CUnknown::NonDelegatingAddRef
// ---------------------------------------------------------------------------
STDMETHODIMP_(ULONG) CUnknown::NonDelegatingAddRef()
{
    LONG count = InterlockedIncrement(&m_lRefCount);
    return static_cast<ULONG>(count);
}

// ---------------------------------------------------------------------------
// CUnknown::NonDelegatingRelease
// When the count reaches zero the object deletes itself.
// ---------------------------------------------------------------------------
STDMETHODIMP_(ULONG) CUnknown::NonDelegatingRelease()
{
    LONG count = InterlockedDecrement(&m_lRefCount);
    if (count == 0) {
        delete this;
    }
    return static_cast<ULONG>(count);
}

// ---------------------------------------------------------------------------
// CUnknown::NonDelegatingQueryInterface
// Base implementation — always returns STATUS_NOINTERFACE.
// Derived classes override this via DECLARE_STD_UNKNOWN / their own impl.
// ---------------------------------------------------------------------------
STDMETHODIMP_(NTSTATUS) CUnknown::NonDelegatingQueryInterface(REFIID rIID, PVOID* ppVoid)
{
    UNREFERENCED_PARAMETER(rIID);
    *ppVoid = nullptr;
    return STATUS_NOINTERFACE;
}
