#pragma once

#include <Types.h>

typedef struct {
    UINT8 Hh;
    UINT8 Mm;
    UINT8 Ss;
    UINT32 Epoch;
} ClockTime;

VOID ClockTick(VOID);
VOID FormatClock(CHAR *Buffer, ClockTime T);
VOID InitSystemClock(VOID);
