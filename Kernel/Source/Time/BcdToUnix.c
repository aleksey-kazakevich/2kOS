#include <Time/Timer.h>

UINT8 BcdToBin(UINT8 Bcd) {
    return (Bcd & 0x0F) + ((Bcd >> 4) * 10);
}

UINT8 BinToBcd(UINT8 Bin) {
    return ((Bin / 10) << 4) | (Bin % 10);
}

static BOOL IsLeap(UINT32 Year) {
    return (Year % 400 == 0) || (Year % 4 == 0 && Year % 100 != 0);
}

static UINT8 DaysInMonth(UINT32 Year, UINT8 Month) {
    static const UINT8 Days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (Month == 2 && IsLeap(Year)) return 29;
    return Days[Month-1];
}

UnixTime TimeToUnix(DateTime *T) {
    if (!T) return 0;
    
    UINT64 Days = 0;
    for (UINT32 Y = 1970; Y < T->Year; Y++)
        Days += IsLeap(Y) ? 366 : 365;
    
    for (UINT8 M = 1; M < T->Month; M++)
        Days += DaysInMonth(T->Year, M);
    
    Days += (T->Day - 1);
    
    return Days * 86400ULL + T->Hour * 3600 + T->Minute * 60 + T->Second;
}

VOID UnixToTime(UnixTime Ut, DateTime *T) {
    if (!T) return;
    
    UINT32 Days = Ut / 86400;
    UINT32 Rem = Ut % 86400;
    
    T->Hour = Rem / 3600;
    T->Minute = (Rem % 3600) / 60;
    T->Second = Rem % 60;
    
    UINT32 Year = 1970;
    while (Days >= (IsLeap(Year) ? 366 : 365)) {
        Days -= IsLeap(Year) ? 366 : 365;
        Year++;
    }
    T->Year = Year;
    
    for (T->Month = 1; T->Month <= 12; T->Month++) {
        UINT8 Dim = DaysInMonth(Year, T->Month);
        if (Days < Dim) {
            T->Day = Days + 1;
            break;
        }
        Days -= Dim;
    }
}