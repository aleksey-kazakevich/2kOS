#include <Time/Timer.h>
#include <Drivers/Apic/Apic.h>
#include <Drivers/Apic/ApicRegs.h>
#include <Drivers/Acpi/Acpi.h>
#include <Drivers/Hpet.h>
#include <Return.h>
#include <Asm/Cpu.h>
#include <Asm/Io.h>
#include <Time/Clock.h>
#include <Drivers/Apic/Ioapic.h>

volatile UINT32 Seconds = 0;
BOOL TimerInitialized = FALSE;

#define TIMER_VECTOR        0x20
#define PIT_BASE_FREQ       1193180U
#define PIT_MAX_COUNT       0xFFFFU
#define APIC_DIVISOR        16
#define CALIBRATION_MS      10
#define CALIBRATION_LOOPS   2
#define HPET_CALIB_MS       10
#define VERIFY_MS           5

struct {
    UINT64 Ticks;
    UINT32 Period; 
    UINT32 TscKhz;
    UINT32 ApicBusKhz;
    UINT32 TickHz;
    UINT32 Divider;
    UINT32 InitCount;
    BOOL Calibrated;
} GTimer = {0};


static inline VOID PitSetMode(UINT8 Channel, UINT8 Mode, UINT8 Access);
static inline VOID PitWriteCounter(UINT16 Count);
static inline UINT16 PitReadCounter(VOID);
static inline UINT16 PitReadCounterVerified(VOID);
static VOID PitBusyWait(UINT32 Ms);
static VOID IoBusyWait(UINT32 Iterations);
static UINT32 TimerTscCalibratePrecise(VOID);
static UINT32 TimerApicCalibrateWithTsc(UINT32 ExpectedMs);
static UINT32 TimerApicCalibrateFromFadt(VOID);
static BOOL TimerVerifyCalibrationFast(UINT32 VerifyMs);
static BOOL TimerVerifyCalibration(VOID);


static VOID IoBusyWait(UINT32 Iterations) {
    for (UINT32 I = 0; I < Iterations; I++) {
        Outb(0x80, 0);
    }
}

static inline VOID PitSetMode(UINT8 Channel, UINT8 Mode, UINT8 Access) {
    UINT8 Cmd = (Channel << 6) | (Access << 4) | (Mode << 1);
    Outb(0x43, Cmd);
    IoBusyWait(10);
}

static inline VOID PitWriteCounter(UINT16 Count) {
    Outb(0x40, Count & 0xFF);
    IoBusyWait(10);
    Outb(0x40, (Count >> 8) & 0xFF);
    IoBusyWait(10);
}

static inline UINT16 PitReadCounter(VOID) {
    Outb(0x43, 0x80);
    IoBusyWait(10);
    UINT8 Low = Inb(0x40);
    IoBusyWait(10);
    UINT8 High = Inb(0x40);
    IoBusyWait(10);
    return (UINT16)(Low | (High << 8));
}

static inline UINT16 PitReadCounterVerified(VOID) {
    UINT16 Val1, Val2;
    INT Attempts = 0;
    
    do {
        Val1 = PitReadCounter();
        Val2 = PitReadCounter();
        Attempts++;

        if (Attempts > 10) {
            return Val1;
        }

    } while (Val1 > Val2 + 10 || Val2 > Val1 + 10);
    
    return (Val1 + Val2) / 2;
}

static VOID PitBusyWait(UINT32 Ms) {
    PitSetMode(0, 0, 3);
    
    UINT32 PitTicks = (PIT_BASE_FREQ * Ms) / 1000;
    if (PitTicks > PIT_MAX_COUNT) PitTicks = PIT_MAX_COUNT;
    if (PitTicks < 100) PitTicks = 100;
    
    PitWriteCounter((UINT16)PitTicks);

    UINT16 Curr;
    do {
        Curr = PitReadCounter();
        IoBusyWait(10);
    } while (Curr > 1);
}


static UINT32 TimerTscCalibratePrecise(VOID) {
    UINT64 TscStart, TscEnd;
    UINT64 TotalTsc = 0;
    UINT32 ValidMeasurements = 0;
    
    for (INT Sample = 0; Sample < CALIBRATION_LOOPS; Sample++) {
        UINT16 PitInitial = PIT_MAX_COUNT;
        
        PitSetMode(0, 0, 3);
        PitWriteCounter(PitInitial);
        
        IoBusyWait(100);

        TscStart = ReadTimeStampCounter();
        
        UINT16 PrevCount = PitInitial;
        UINT16 CurrCount;
        UINT32 StallCount = 0;
        
        do {
            CurrCount = PitReadCounterVerified();
            
            if (CurrCount > PrevCount) {
                break;
            }
            
            PrevCount = CurrCount;

            IoBusyWait(10);
            
            StallCount++;
            if (StallCount > 1000000) {
                break;
            }
        } while (CurrCount > 0);
        
        TscEnd = ReadTimeStampCounter();
        
        if (CurrCount == 0 || StallCount < 1000000) {
            UINT64 TscElapsed = TscEnd - TscStart;
            UINT64 Freq = (TscElapsed * PIT_BASE_FREQ) / PIT_MAX_COUNT;
            if (Freq > 100000000ULL && Freq < 10000000000ULL) {
                TotalTsc += Freq;
                ValidMeasurements++;
            }
        }
        PitBusyWait(10);
    }
    
    if (ValidMeasurements == 0) {
        return 2000000;
    }
    
    return (UINT32)(TotalTsc / ValidMeasurements / 1000);
}

static UINT32 TimerApicCalibrateWithTsc(UINT32 ExpectedMs) {
    UINT32 ApicStart, ApicEnd;
    UINT64 TscStart, TscEnd;
    UINT32 TotalApicTicks = 0;
    UINT32 ValidMeasurements = 0;
    
    for (INT Sample = 0; Sample < CALIBRATION_LOOPS; Sample++) {
        ApicWriteReg(LAPIC_TIMER_DIV, TIMER_DIV_16);
        ApicWriteReg(LAPIC_TIMER_INITCNT, 0);
        IoBusyWait(100);
        ApicWriteReg(LAPIC_TIMER_INITCNT, 0xFFFFFFFFU);
        IoBusyWait(100);
        ApicStart = ApicReadReg(LAPIC_TIMER_CURRCNT);
        TscStart = ReadTimeStampCounter();
        UINT64 TscTarget = TscStart + ((UINT64)GTimer.TscKhz * ExpectedMs);
        
        do {
            TscEnd = ReadTimeStampCounter();
            IoBusyWait(1);
        } while (TscEnd < TscTarget);

        ApicEnd = ApicReadReg(LAPIC_TIMER_CURRCNT);
        ApicWriteReg(LAPIC_TIMER_INITCNT, 0);
        UINT32 ApicElapsed;
        if (ApicStart >= ApicEnd) {
            ApicElapsed = ApicStart - ApicEnd;
        } else {
            ApicElapsed = (0xFFFFFFFFU - ApicEnd) + ApicStart + 1;
        }
        UINT64 TscElapsed = TscEnd - TscStart;
        UINT64 ExpectedApicTicks = (UINT64)ApicElapsed;
        if (TscElapsed > (UINT64)GTimer.TscKhz * ExpectedMs * 2) {
            UINT64 EstFreq = (UINT64)ApicElapsed * 1000 / ExpectedMs;
            while (EstFreq < 1000000 && ExpectedApicTicks < 0x200000000ULL) {
                ExpectedApicTicks += 0x100000000ULL;
                EstFreq = ExpectedApicTicks * 1000 / ExpectedMs;
            }
            ApicElapsed = (UINT32)ExpectedApicTicks;
        }
        if (ApicElapsed > 1000 && ApicElapsed < 0x20000000) {
            TotalApicTicks += ApicElapsed;
            ValidMeasurements++;
        }
        PitBusyWait(20);
    }
    
    if (ValidMeasurements == 0) {
        return 0;
    }
    UINT32 AvgApicTicks = TotalApicTicks / ValidMeasurements;
    UINT32 Period = (AvgApicTicks * APIC_DIVISOR * 1000) / ExpectedMs;
    
    return Period;
}


static UINT32 TimerApicCalibrateFromFadt(VOID) {
    Acpi *AcpiTable = AcpiGet();

    if (!AcpiTable || AcpiTable->ApicBusFreq == 0) {
        return 0;
    }
    return AcpiTable->ApicBusFreq;
}

static BOOL TimerVerifyCalibrationFast(UINT32 VerifyMs) {
    UINT32 TestHz = 1000;
    UINT32 InitVal;
    UINT64 StartTicks;
    UINT64 ElapsedTicks;
    UINT32 ExpectedTicks;
    UINT32 MinTicks;
    UINT32 MaxTicks;

    if (GTimer.Period == 0 || VerifyMs == 0) {
        return FALSE;
    }

    InitVal = (UINT32)((UINT64)GTimer.Period / ((UINT64)TestHz * APIC_DIVISOR));
    if (InitVal < 2) {
        return FALSE;
    }

    ApicWriteReg(LAPIC_TIMER_DIV, TIMER_DIV_16);
    ApicWriteReg(LAPIC_TIMER_INITCNT, 0);
    StartTicks = GTimer.Ticks;

    ApicWriteReg(LAPIC_LVT_TIMER, TIMER_VECTOR | TIMER_PERIODIC);
    ApicWriteReg(LAPIC_TIMER_INITCNT, InitVal);

    if (HpetIsAvailable()) {
        HpetDelayMs(VerifyMs);
    } else {
        UINT64 TscStart = ReadTimeStampCounter();
        UINT64 TscTarget = TscStart + ((UINT64)GTimer.TscKhz * VerifyMs);
        while (ReadTimeStampCounter() < TscTarget) {
            IoBusyWait(1);
        }
    }

    ApicWriteReg(LAPIC_LVT_TIMER, LVT_MASKED);
    ApicWriteReg(LAPIC_TIMER_INITCNT, 0);

    ElapsedTicks = GTimer.Ticks - StartTicks;
    ExpectedTicks = (TestHz * VerifyMs) / 1000;
    if (ExpectedTicks == 0) {
        ExpectedTicks = 1;
    }
    MinTicks = ExpectedTicks * 90 / 100;
    MaxTicks = ExpectedTicks * 110 / 100;

    return (ElapsedTicks >= MinTicks && ElapsedTicks <= MaxTicks);
}

static BOOL TimerVerifyCalibration(VOID) {
    return TimerVerifyCalibrationFast(50);
}

VOID TimerInit(UINT32 DesiredFreq) {
    BOOL HpetReady;
    BOOL UsedFadtFreq;
    UINT32 Period = 0;

    if (DesiredFreq == 0) DesiredFreq = 1000;

    HpetReady = (HpetInit() == SUCCESS);

    if (HpetReady) {
        GTimer.TscKhz = (UINT32)HpetCalibrateTsc();
    }
    if (GTimer.TscKhz == 0) {
        GTimer.TscKhz = TimerTscCalibratePrecise();
    }

    Period = TimerApicCalibrateFromFadt();
    UsedFadtFreq = (Period > 0);
    if (!UsedFadtFreq && HpetReady) {
        Period = HpetCalibrateApicTimer(HPET_CALIB_MS);
    }
    if (Period == 0) {
        Period = TimerApicCalibrateWithTsc(HPET_CALIB_MS);
    }
    if (Period == 0) {
        Period = TimerApicCalibrateWithTsc(50);
    }

    if (Period == 0) {
        GTimer.Period = 2000000;
        GTimer.Calibrated = FALSE;
    } else {
        GTimer.Period = Period;
        GTimer.Calibrated = TRUE;
    }

    GTimer.ApicBusKhz = (UINT32)((UINT64)GTimer.Period * 1000 / APIC_DIVISOR / 1000);
    if (GTimer.Calibrated && !UsedFadtFreq) {
        TimerVerifyCalibrationFast(VERIFY_MS);
    }
    UINT64 Divisor = (UINT64)DesiredFreq * APIC_DIVISOR;
    UINT32 InitValue;
    
    if (Divisor == 0) {
        InitValue = 0xFFFFFFFFU;
    } else {
        UINT64 Temp = (UINT64)GTimer.Period / Divisor;
        if (Temp > 0xFFFFFFFFU) {
            InitValue = 0xFFFFFFFFU;
        } else {
            InitValue = (UINT32)Temp;
        }
    }
    
    if (InitValue < 2) InitValue = 2;
    GTimer.InitCount = InitValue;
    GTimer.TickHz = DesiredFreq ? DesiredFreq : 1000;
    ApicWriteReg(LAPIC_TIMER_DIV, TIMER_DIV_16);
    ApicWriteReg(LAPIC_LVT_TIMER, TIMER_VECTOR | TIMER_PERIODIC);
    ApicWriteReg(LAPIC_TIMER_INITCNT, InitValue);
    TimerInitialized = TRUE;
}

VOID TimerHandler(VOID) {
    GTimer.Ticks++;
    if (GTimer.Ticks >= GTimer.TickHz) {
        GTimer.Ticks = 0;
        Seconds++;
        ClockTick();
    }
    
    ApicEoi();
}

UINT64 TimerTicks(VOID) {
    if (HpetIsAvailable()) {
        return HpetReadCounter();
    }
    return GTimer.Ticks;
}

UINT32 TimerFreq(VOID) {
    if (HpetIsAvailable()) {
        return (UINT32)HpetGetFrequency();
    }
    if (GTimer.TickHz) {
        return GTimer.TickHz;
    }
    return 1000;
}

UINT32 TimerApicCurrentFreq(VOID) {
    if (GTimer.InitCount == 0) return 0;
    UINT64 Freq = (UINT64)GTimer.Period / ((UINT64)GTimer.InitCount * APIC_DIVISOR);
    
    if (Freq > 0xFFFFFFFFU) Freq = 0xFFFFFFFFU;
    
    return (UINT32)Freq;
}

UINT32 TimerApicMs(VOID) {
    UINT32 CurrentFreq = TimerApicCurrentFreq();
    if (CurrentFreq == 0) return 0;
    
    return (UINT32)(GTimer.Ticks * 1000 / CurrentFreq);
}

UINT32 TimerTicksPerMs(VOID) {
    if (HpetIsAvailable()) {
        return (UINT32)(HpetGetFrequency() / 1000);
    }
    if (GTimer.TickHz) {
        return GTimer.TickHz / 1000;
    }
    return 1;
}

UINT64 TimerTscFreq(VOID) {
    return (UINT64)GTimer.TscKhz * 1000;
}
