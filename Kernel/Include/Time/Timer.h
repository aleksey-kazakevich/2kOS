#pragma once

#include <Types.h>

typedef struct {
    UINT32 Year;
    UINT8  Month;
    UINT8  Day;
    UINT8  Hour;
    UINT8  Minute;
    UINT8  Second;
} DateTime;

typedef UINT64 UnixTime;

UINT8 BcdToBin(UINT8 Bcd);
UINT8 BinToBcd(UINT8 Bin);
UnixTime TimeToUnix(DateTime *T);
VOID UnixToTime(UnixTime Ut, DateTime *T);

INT RtcReadTime(DateTime *T);

VOID TimerInit(UINT32 Freq);
VOID InitTimer(VOID);
UINT64 TimerTicks(VOID);
UINT32 TimerFreq(VOID);
UINT32 TimerApicMs(VOID);
UINT32 TimerTicksPerMs(VOID);
UINT64 TimerTscFreq(VOID);

static inline UINT64 TimerMsToTicks(UINT32 Ms) {
    UINT32 Tpm = TimerTicksPerMs();
    if (Tpm == 0) {
        Tpm = 1;
    }
    return (UINT64)Ms * Tpm;
}

VOID TimerUdelay(UINT32 Us);
VOID TimerMdelay(UINT32 Ms);
VOID TimerSdelay(UINT32 S);
VOID TimerSleep(UINT32 Ms);

typedef enum {
    TIMER_SOURCE_HPET,
    TIMER_SOURCE_APIC,
    TIMER_SOURCE_PIT,
} TimerSource;
