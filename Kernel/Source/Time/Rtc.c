#include <Time/Rtc.h>
#include <Asm/Cpu.h>
#include <Asm/Io.h>

static VOID RtcWaitReady(VOID) {
    while (1) {
        Outb(0x70, 0x0A);
        if (!(Inb(0x71) & 0x80))
            break;
    }
}

static UINT8 CmosRead(UINT8 Reg) {
    Outb(0x70, Reg);
    return Inb(0x71);
}

static UINT8 BcdToBin(UINT8 Bcd) {
    return (Bcd & 0x0F) + (Bcd >> 4) * 10;
}

VOID ReadRtcTime(UINT32 *Hour, UINT32 *Minute, UINT32 *Second) {
    RtcWaitReady();

    UINT8 Ss = CmosRead(0x00);
    UINT8 Mm = CmosRead(0x02);
    UINT8 Hh = CmosRead(0x04);
    UINT8 RegB = CmosRead(0x0B);

    UINT8 IsPm = Hh & 0x80;

    if (!(RegB & 0x04)) {
        Ss = BcdToBin(Ss);
        Mm = BcdToBin(Mm);
        Hh = BcdToBin(Hh & 0x7F);
    }
    else {
        Hh &= 0x7F;
    }

    if (!(RegB & 0x02) && IsPm) {
        Hh = (Hh + 12) % 24;
    }

    Hh = (UINT8)((Hh + TIMEZONE_OFFSET + 24) % 24);

    *Hour = Hh;
    *Minute = Mm;
    *Second = Ss;
}

VOID ReadRtcDate(UINT32 *Year, UINT32 *Month, UINT32 *Day) {
    RtcWaitReady();

    UINT8 DayReg = CmosRead(0x07);
    UINT8 MonthReg = CmosRead(0x08);
    UINT8 YearReg = CmosRead(0x09);
    UINT8 CenturyReg = CmosRead(0x32);
    UINT8 RegB = CmosRead(0x0B);

    if (!(RegB & 0x04))
    {
        DayReg = BcdToBin(DayReg);
        MonthReg = BcdToBin(MonthReg);
        YearReg = BcdToBin(YearReg);
        CenturyReg = BcdToBin(CenturyReg);
    }

    *Day = DayReg;
    *Month = MonthReg;
    
    if (CenturyReg >= 19) {
        *Year = CenturyReg * 100 + YearReg;
    } else {
        *Year = 2000 + YearReg;
    }
}