#pragma once

#include <Types.h>

#define TIMEZONE_OFFSET 7

VOID ReadRtcTime(UINT32 *Hour, UINT32 *Minute, UINT32 *Second);
VOID ReadRtcDate(UINT32 *Year, UINT32 *Month, UINT32 *Day);
