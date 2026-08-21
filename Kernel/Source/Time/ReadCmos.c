#include <Time/Timer.h>
#include <Asm/Io.h>
#include <Return.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define CMOS_SEC   0x00
#define CMOS_MIN   0x02
#define CMOS_HOUR  0x04
#define CMOS_DAY   0x07
#define CMOS_MONTH 0x08
#define CMOS_YEAR  0x09
#define CMOS_CENT  0x32
#define CMOS_STAT_B 0x0B

static UINT8 CmosRead(UINT8 Reg) {
    Outb(CMOS_ADDR, Reg);
    return Inb(CMOS_DATA);
}

INT RtcReadTime(DateTime *T) {
    if (!T) RETURN(NO_OBJECT);
    
    UINT8 B = CmosRead(CMOS_STAT_B);
    BOOL Bcd = !(B & 0x04);
    
    T->Second = CmosRead(CMOS_SEC);
    T->Minute = CmosRead(CMOS_MIN);
    T->Hour   = CmosRead(CMOS_HOUR);
    T->Day    = CmosRead(CMOS_DAY);
    T->Month  = CmosRead(CMOS_MONTH);
    T->Year   = CmosRead(CMOS_YEAR);
    UINT8 Cent = CmosRead(CMOS_CENT);
    
    if (Bcd) {
        T->Second = BcdToBin(T->Second);
        T->Minute = BcdToBin(T->Minute);
        T->Hour   = BcdToBin(T->Hour);
        T->Day    = BcdToBin(T->Day);
        T->Month  = BcdToBin(T->Month);
        T->Year   = BcdToBin(T->Year);
        Cent      = BcdToBin(Cent);
    }
    
    T->Year += (Cent ? Cent * 100 : 2000);
    RETURN(SUCCESS);
}
