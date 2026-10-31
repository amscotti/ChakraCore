//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "Backend.h"

// Conditionally-compiled on x64 and arm/arm64
#if PDATA_ENABLED

#ifdef _WIN32
// ----------------------------------------------------------------------------
//  _WIN32 x64 unwind uses PDATA
// ----------------------------------------------------------------------------

void PDataManager::RegisterPdata(RUNTIME_FUNCTION* pdataStart, _In_ const ULONG_PTR functionStart, _In_ const ULONG_PTR functionEnd, _Out_ PVOID* pdataTable, ULONG entryCount, ULONG maxEntryCount)
{
    BOOLEAN success = FALSE;
    HRESULT hr = S_OK;
    if (AutoSystemInfo::Data.IsWin8OrLater())
    {
        Assert(pdataTable != NULL);

        // Since we do not expect many thunk functions to be created, we are using 1 table/function
        // for now. This can be optimized further if needed.
        NTSTATUS status = NtdllLibrary::Instance->AddGrowableFunctionTable(pdataTable,
            pdataStart,
            entryCount,
            maxEntryCount,
            /*RangeBase*/ functionStart,
            /*RangeEnd*/ functionEnd);
        success = NT_SUCCESS(status);
        Assert(!success || pdataTable);
        hr = status;
    }
    else
    {
        *pdataTable = pdataStart;
        success = RtlAddFunctionTable(pdataStart, entryCount, functionStart);
        if (!success) 
        {
            hr = E_OUTOFMEMORY; // only OOM error can happen for RtlAddFunctionTable
        }
    }
    if (!success)
    {
        Js::Throw::XDataRegistrationError(hr, functionStart);
    }
}

void PDataManager::UnregisterPdata(RUNTIME_FUNCTION* pdata)
{
    if (AutoSystemInfo::Data.IsWin8OrLater())
    {
        NtdllLibrary::Instance->DeleteGrowableFunctionTable(pdata);
    }
    else
    {
        BOOLEAN success = RtlDeleteFunctionTable(pdata);
        Assert(success);
    }
}

#else  // !_WIN32

// ----------------------------------------------------------------------------
//  !_WIN32 x64 unwind uses .eh_frame. ARM64 still emits Windows unwind codes.
// ----------------------------------------------------------------------------

void PDataManager::RegisterPdata(RUNTIME_FUNCTION* pdataStart,
    _In_ const ULONG_PTR functionStart, _In_ const ULONG_PTR functionEnd,
    _Out_ PVOID* pdataTable, ULONG entryCount, ULONG maxEntryCount)
{
#if defined(__APPLE__) && defined(_M_ARM64)
    // Apple registers one FDE per call. The buffer is a CIE followed by one
    // FDE; mac_fde_wrapper skips the CIE (id 0). These bytes stay readable
    // for the life of the thunk block. Close() unregisters before decommit.
    typedef void (*mac_fde_reg_op)(const void* addr);
    extern void mac_fde_wrapper(const char* dataStart, mac_fde_reg_op reg_op);
    (void)functionStart;
    (void)functionEnd;
    (void)entryCount;
    (void)maxEntryCount;
    mac_fde_wrapper((const char*)pdataStart, __register_frame);
#else
    __REGISTER_FRAME(pdataStart);
#endif
    *pdataTable = pdataStart;
}

void PDataManager::UnregisterPdata(RUNTIME_FUNCTION* pdata)
{
#if defined(__APPLE__) && defined(_M_ARM64)
    typedef void (*mac_fde_reg_op)(const void* addr);
    extern void mac_fde_wrapper(const char* dataStart, mac_fde_reg_op reg_op);
    mac_fde_wrapper((const char*)pdata, __deregister_frame);
#else
    __DEREGISTER_FRAME(pdata);
#endif
}

#endif  // !_WIN32
#endif  // PDATA_ENABLED
