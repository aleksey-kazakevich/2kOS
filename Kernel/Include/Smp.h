#pragma once

#include <PerCpu.h>
#include <Types.h>

/*
 * Public per-CPU snapshot. Shared between kernel and userspace.
 * Must stay binary-compatible: userspace smpinfo.c mirrors it.
 */
typedef struct {
    UINT32 CpuIndex;
    UINT32 LapicId;
    UINT32 AcpiUid;
    UINT32 Online;
    UINT32 IsBsp;
    UINT32 Idle;
    UINT64 Ticks;
    UINT64 IpiCount;
    UINT64 Load;
    UINT64 MinVruntime;
    UINT32 RunQueueCount;
    UINT32 CurrentPid;     /* -1 if none */
    CHAR   CurrentName[32];
} SmpCpuInfo;

/* Fill *Out with a snapshot of CPU Index.
 * Returns SUCCESS, NOT_FOUND, or INCORRECT_VALUE. */
INT SmpGetCpuInfo(UINT32 Index, SmpCpuInfo *Out);

/* Number of CPUs (both online and offline). */
UINT32 SmpGetCpuCount(VOID);

INT  SmpInit(VOID);
BOOL SmpIsReady(VOID);
VOID StopCpus(VOID);
VOID StopOtherCpus(VOID);

// For convenience, how many CPUs are online
UINT32 SmpGetOnlineCount(VOID);