#include <Time/Timer.h>
#include <Asm/Cpu.h>
#include <Asm/Io.h>
#include <Drivers/Hpet.h>

static UINT64 TscPerMs = 0;
static UINT64 TscKhz = 0;

static VOID TscCalibrateWithPit(VOID) {
    Outb(0x43, 0xB0);
    Outb(0x42, 0xFF);
    Outb(0x42, 0xFF);

    UINT64 Start = ReadTimeStampCounter();
    for (INT I = 0; I < 10000; I++) {
        Inb(0x61);
    }
    UINT64 End = ReadTimeStampCounter();

    TscPerMs = (End - Start) / 10;
    TscKhz = TscPerMs;
}

static VOID TscCalibrateFixed(VOID) {
    TscPerMs = 2000000;
    TscKhz = 2000000;
}

VOID TimerUdelay(UINT32 Us) {
    if (!TscPerMs) {
        TscCalibrateWithPit();
    }

    if (!TscPerMs) {
        TscCalibrateFixed();
    }

    UINT64 Start = ReadTimeStampCounter();
    UINT64 Need = (TscPerMs * Us) / 1000;
    while (ReadTimeStampCounter() - Start < Need) {
        CpuPause();
    }
}

VOID TimerMdelay(UINT32 Ms) {
    TimerUdelay(Ms * 1000);
}

VOID TimerSdelay(UINT32 S) {
    TimerMdelay(S * 1000);
}

VOID TimerSleep(UINT32 Ms) {
    UINT32 Freq;
    UINT64 Start;
    UINT64 Need;

    if (Ms == 0) {
        return;
    }

    Freq = TimerFreq();
    if (Freq == 0) {
        TimerMdelay(Ms);
        return;
    }

    Start = TimerTicks();
    Need = ((UINT64)Ms * Freq) / 1000;
    if (Need == 0) {
        Need = 1;
    }

    while ((TimerTicks() - Start) < Need) {
        Halt();
    }
}
