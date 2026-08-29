#pragma once

#include <Types.h>

// Initialization
INT HpetInit(VOID);
BOOL HpetIsAvailable(VOID);

// Reading the counter
UINT64 HpetReadCounter(VOID);
UINT64 HpetGetFrequency(VOID);
UINT64 HpetGetPeriod(VOID);
UINT64 HpetGetNanoseconds(VOID);
UINT64 HpetGetMicroseconds(VOID);

// Delays
VOID HpetDelayUs(UINT32 Us);
VOID HpetDelayMs(UINT32 Ms);

// APIC and TSC Calibration
UINT32 HpetCalibrateApicTimer(UINT32 DesiredMs);
UINT64 HpetCalibrateTsc(VOID);
