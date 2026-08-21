#include <Time/Clock.h>
#include <Time/Rtc.h>

volatile ClockTime SystemClock = {0, 0, 0, 0}; //start from 00:00:00

static const UINT8 DaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static INT IsLeapYear(INT Year) {
    return (Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0));
}

static UINT32 RtcToEpoch(UINT32 Year, UINT32 Month, UINT32 Day,
                             UINT32 Hour, UINT32 Minute, UINT32 Second) {
    UINT32 Epoch = 0;
    
    for (UINT32 Y = 1970; Y < Year; Y++) {
        Epoch += IsLeapYear(Y) ? 366 : 365;
    }
    
    for (UINT32 M = 1; M < Month; M++) {
        Epoch += DaysInMonth[M-1];
        if (M == 2 && IsLeapYear(Year)) Epoch++;
    }
    
    Epoch += Day - 1;
    
    Epoch = Epoch * 86400 + Hour * 3600 + Minute * 60 + Second;
    
    return Epoch;
}

VOID InitSystemClock(VOID) {
    UINT32 H, M, S;
    ReadRtcTime(&H, &M, &S);
    
    UINT32 Year, Month, Day;
    ReadRtcDate(&Year, &Month, &Day);
    
    SystemClock.Hh = (UINT8)H;
    SystemClock.Mm = (UINT8)M;
    SystemClock.Ss = (UINT8)S;
    SystemClock.Epoch = RtcToEpoch(Year, Month, Day, H, M, S);
}

VOID ClockTick(VOID) {
    SystemClock.Ss++;
    SystemClock.Epoch++;

    if (SystemClock.Ss >= 60)
    {
        SystemClock.Ss = 0;
        SystemClock.Mm++;

        if (SystemClock.Mm >= 60)
        {
            SystemClock.Mm = 0;
            SystemClock.Hh++;

            if (SystemClock.Hh >= 24)
            {
                SystemClock.Hh = 0;
            }
        }
    }
}

VOID FormatClock(CHAR *Buffer, ClockTime T) {
    Buffer[0] = '0' + (T.Hh / 10);
    Buffer[1] = '0' + (T.Hh % 10);
    Buffer[2] = ':';
    Buffer[3] = '0' + (T.Mm / 10);
    Buffer[4] = '0' + (T.Mm % 10);
    Buffer[5] = ':';
    Buffer[6] = '0' + (T.Ss / 10);
    Buffer[7] = '0' + (T.Ss % 10);
    Buffer[8] = '\0';
}