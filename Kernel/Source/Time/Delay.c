#include <Drivers/Hpet.h>
#include <Time/Timer.h>
#include <Asm/Cpu.h>
#include <Asm/Io.h>

STATIC UINT64 TscPerMs = 0;

/* ============================================================================
 * PIT calibration
 * ============================================================================ */

#define PIT_FREQ 1193182ULL
#define PIT_CH0  0x40
#define PIT_CMD  0x43

// One-shot mode (mode 0), 16-bit access
STATIC VOID PitPrepareOneshot(VOID) {
    // Channel 0, lobyte/hibyte, mode 0 (interrupt on terminal count), binary
    Outb(PIT_CMD, 0x30);
}

STATIC VOID PitSetCount(UINT16 Count) {
    Outb(PIT_CH0, (UINT8)(Count & 0xFF));
    Outb(PIT_CH0, (UINT8)((Count >> 8) & 0xFF));
}

// Reads the current PIT counter value (via latch)
STATIC UINT16 PitReadCount(VOID) {
    // Latch channel 0: 0b00000000 → channel 0, latch command
    Outb(PIT_CMD, 0x00);
    UINT8 Lo = Inb(PIT_CH0);
    UINT8 Hi = Inb(PIT_CH0);
    return (UINT16)(Lo | ((UINT16)Hi << 8));
}

// Calibration via PIT: start the counter for N ms, measure TSC
STATIC BOOL TscCalibrateWithPit(VOID) {
    const UINT32 CalMs = 50;                    // 50 ms — accurate enough
    const UINT32 PitTicks = (PIT_FREQ * CalMs) / 1000;

    // PIT 16-bit, max 65535
    if (PitTicks == 0 || PitTicks > 0xFFFF) {
        return FALSE;
    }

    // Disable the speaker / port B (not critical, but let it be)
    UINT8 Port61 = Inb(0x61);
    Outb(0x61, (Port61 & 0xFC) | 0x01);    // enable timer 2 gate
    Outb(0x61, Port61);                     // restore

    // Configure the PIT for one-shot
    PitPrepareOneshot();
    PitSetCount((UINT16)PitTicks);

    // Wait until the counter reaches 0
    UINT64 TscStart = ReadTimeStampCounter();

    // Wait for completion: read the counter until it becomes small
    // The PIT counter counts DOWN. We wait until it reaches 0.
    UINT16 Prev = 0xFFFF;
    UINT32 Stall = 0;
    while (1) {
        UINT16 Curr = PitReadCount();
        if (Curr == 0) break;               // counter reached 0

        // If the value suddenly increased — we caught a reload
        if (Curr > Prev + 100) break;

        Prev = Curr;

        if (++Stall > 100000000) {
            return FALSE;                   // hang protection
        }
    }

    UINT64 TscEnd = ReadTimeStampCounter();
    UINT64 TscDelta = TscEnd - TscStart;

    if (TscDelta < 100000) {
        return FALSE;                       // too small — something is wrong
    }

    // TscPerMs = TscDelta / CalMs
    TscPerMs = TscDelta / CalMs;

    // Sanity check: from 100 MHz to 10 GHz
    if (TscPerMs < 100000 || TscPerMs > 10000000) {
        TscPerMs = 0;
        return FALSE;
    }

    return TRUE;
}

/* ============================================================================
 * Initialization (called once at timer startup)
 * ============================================================================ */

VOID TimerDelayInit(VOID) {
    if (TscPerMs != 0) return;

    // 1. Try HPET
    if (HpetIsAvailable()) {
        UINT64 TscFreq = HpetCalibrateTsc();    // returns Hz
        if (TscFreq > 0) {
            TscPerMs = TscFreq / 1000;
            return;
        }
    }

    // 2. Fallback to PIT
    if (TscCalibrateWithPit()) {
        return;
    }

    // 3. Absolute last resort — but without magic 2 GHz.
    //    Leave TscPerMs = 0 and use HpetDelayMs
    //    directly if HPET is available.
    TscPerMs = 0;
}

/* ============================================================================
 * Delays
 * ============================================================================ */

VOID TimerUdelay(UINT32 Us) {
    if (Us == 0) return;

    // If HPET is available — use it (accurate and no calibration needed)
    if (HpetIsAvailable()) {
        HpetDelayUs(Us);
        return;
    }

    // Otherwise — TSC
    if (TscPerMs == 0) {
        TimerDelayInit();
    }

    if (TscPerMs == 0) {
        // TSC not calibrated — rough delay via a loop
        // Estimate the speed: ~1000 iterations per 1 us at 2 GHz.
        // This is bad, but better than nothing.
        UINT64 Loops = (UINT64)Us * 1000;
        while (Loops--) {
            CpuPause();
        }
        return;
    }

    UINT64 Start = ReadTimeStampCounter();
    UINT64 Need = (TscPerMs * Us) / 1000;
    if (Need == 0) Need = 1;

    while (ReadTimeStampCounter() - Start < Need) {
        CpuPause();
    }
}

VOID TimerMdelay(UINT32 Ms) {
    if (Ms == 0) return;

    // If HPET is available — use it
    if (HpetIsAvailable()) {
        HpetDelayMs(Ms);
        return;
    }

    // Otherwise via microseconds (TSC)
    while (Ms >= 1000) {
        TimerUdelay(1000 * 1000);
        Ms -= 1000;
    }
    if (Ms > 0) {
        TimerUdelay(Ms * 1000);
    }
}

VOID TimerSdelay(UINT32 S) {
    while (S--) {
        TimerMdelay(1000);
    }
}

VOID TimerSleep(UINT32 Ms) {
    if (Ms == 0) return;

    UINT32 Freq = TimerFreq();
    if (Freq == 0) {
        TimerMdelay(Ms);
        return;
    }

    UINT64 Start = TimerTicks();
    UINT64 Need = ((UINT64)Ms * Freq) / 1000;
    if (Need == 0) Need = 1;

    while ((TimerTicks() - Start) < Need) {
        Halt();
    }
}