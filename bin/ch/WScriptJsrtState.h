//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation and contributors. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

// Process-global critical section guarding WScriptJsrt's module/script maps.
// Created on first use and intentionally leaked so teardown never depends on
// C++ global destructor order across agent threads.
static CRITICAL_SECTION* HostGlobalsCS()
{
    static CRITICAL_SECTION* s_cs = [] {
        CRITICAL_SECTION* cs = new CRITICAL_SECTION();
        InitializeCriticalSection(cs);
        return cs;
    }();
    return s_cs;
}

// RAII guard so early returns between map operations cannot strand the lock.
// Never held across engine calls that can re-enter a host fetch callback.
struct HostGlobalsLock
{
    CRITICAL_SECTION* cs;
    HostGlobalsLock() : cs(HostGlobalsCS()) { EnterCriticalSection(cs); }
    ~HostGlobalsLock() { LeaveCriticalSection(cs); }
};

// Leaf critical section serializing stderr emissions across agent threads.
// Intentionally leaked on teardown like HostGlobalsCS(); never held across
// JS/JsRT calls or HostGlobalsLock.
static CRITICAL_SECTION* StderrCS()
{
    static CRITICAL_SECTION* s_cs = [] {
        CRITICAL_SECTION* cs = new CRITICAL_SECTION();
        InitializeCriticalSection(cs);
        return cs;
    }();
    return s_cs;
}

WScriptJsrt::StderrLock::StderrLock()
{
    EnterCriticalSection(StderrCS());
}

WScriptJsrt::StderrLock::~StderrLock()
{
    LeaveCriticalSection(StderrCS());
}

// Negative-phase classification state (host flag -NegativePhaseReport).
// Both flags are std::atomic<bool> because S6e crossthread agents execute and
// report errors concurrently with the main thread; emission uses
// compare_exchange_strong so at most one thread prints CH_NEGATIVE_PHASE.
static std::atomic<bool> s_moduleGraphFailureWitness{ false };
static std::atomic<bool> s_negativePhaseMarkerEmitted{ false };

// Per-thread nesting depth for synchronous JsParseModuleSource calls. Static
// import fetches only occur while t_moduleParseDepth > 0; fetches outside parse
// are runtime dynamic import() requests whose rejections must not poison
// subsequent runtime errors as "resolution".
static THREAD_LOCAL int t_moduleParseDepth = 0;

struct ModuleParseScope
{
    ModuleParseScope() { ++t_moduleParseDepth; }
    ~ModuleParseScope() { --t_moduleParseDepth; }
};

void WScriptJsrt::NoteModuleGraphFailure()
{
    if (HostConfigFlags::flags.NegativePhaseReportIsEnabled)
    {
        s_moduleGraphFailureWitness.store(true);
    }
}

void WScriptJsrt::ClearModuleGraphFailureWitness()
{
    if (HostConfigFlags::flags.NegativePhaseReportIsEnabled)
    {
        s_moduleGraphFailureWitness.store(false);
    }
}

// Emits the single CH_NEGATIVE_PHASE marker line for this run, classified
// from where the failure originated (never from error message text):
//   parse      the root entry point failed in the parse/compile stage
//   resolution a static module-graph failure witnessed while loading/linking
//   runtime    anything else (script/module evaluation, caught dynamic import()
//              followed by a runtime error, message-queue callbacks)
void WScriptJsrt::ReportNegativePhaseMarker(bool parseStageFailure)
{
    if (!HostConfigFlags::flags.NegativePhaseReportIsEnabled)
    {
        return;
    }
    // Hold StderrLock across CAS + emit + fflush so neither CH_NEGATIVE_PHASE
    // itself nor a sibling thread's subsequent PrintException can interleave or
    // print ahead of line 1.
    StderrLock lock;
    bool expected = false;
    if (!s_negativePhaseMarkerEmitted.compare_exchange_strong(expected, true))
    {
        return;
    }
    const char* phase = parseStageFailure ? "parse"
        : (s_moduleGraphFailureWitness.load() ? "resolution" : "runtime");
    fprintf(stderr, "CH_NEGATIVE_PHASE: %s\n", phase);
    fflush(stderr);
}

MessageQueue* WScriptJsrt::GetThreadMessageQueue()
{
    RuntimeThreadData* threadData = GetRuntimeThreadLocalData().threadData;
    if (threadData != nullptr && threadData->messageQueue != nullptr)
    {
        return threadData->messageQueue;
    }
    return messageQueue;
}

DWORD_PTR WScriptJsrt::GetNextSourceContext()
{
    return sourceContext.fetch_add(1);
}

void WScriptJsrt::RegisterScriptDir(DWORD_PTR sourceContext, LPCSTR fullDirNarrow)
{
    HostGlobalsLock lock;
    GetDir(fullDirNarrow, &scriptDirMap[sourceContext]);
}

void WScriptJsrt::GetDir(LPCSTR fullPathNarrow, std::string *fullDirNarrow)
{
    char fileDrive[_MAX_DRIVE];
    char fileDir[_MAX_DIR];

    std::string result;
    if (_splitpath_s(fullPathNarrow, fileDrive, _countof(fileDrive), fileDir, _countof(fileDir), nullptr, 0, nullptr, 0) == 0)
    {
        result += fileDrive;
        result += fileDir;
    }

    *fullDirNarrow = result;
}
