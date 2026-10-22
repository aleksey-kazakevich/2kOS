#pragma once

#include <Types.h>
#include <PerCpu.h>

INT  SmpInit(VOID);
BOOL SmpIsReady(VOID);
VOID StopCpus(VOID);
VOID StopOtherCpus(VOID);

// For convenience, how many CPUs are online
UINT32 SmpGetOnlineCount(VOID);