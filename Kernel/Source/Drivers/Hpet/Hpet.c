#include <Drivers/Hpet.h>
#include <Drivers/Acpi/Acpi.h>
#include <Drivers/Acpi/AcpiTables.h>
#include <Drivers/Apic/Apic.h>
#include <Drivers/Apic/ApicRegs.h>
#include <Asm/Mmio.h>
#include <Asm/Cpu.h>
#include <KDriver.h>
#include <Return.h>
#include <Lib/String.h>

#define HPET_FS_PER_SEC         1000000000000000ULL
#define APIC_TIMER_DIVISOR      16

static struct {
    volatile UINT8 *Base;
    UINT64 FrequencyHz;
    UINT64 PeriodFs;
    BOOL Available;
    KDriver *Driver;
} GHpet = {0};

static volatile UINT64 *HpetReg64(UINT32 Offset) {
    return (volatile UINT64 *)(GHpet.Base + Offset);
}

static BOOL HpetValidateTable(HPET *Table) {
    if (!Table) {
        return FALSE;
    }
    if (AcpiChecksum(Table, Table->Header.Length) != 0) {
        return FALSE;
    }
    if (Table->AddressSpaceId != 0) {
        return FALSE;
    }
    if (Table->BaseAddress == 0) {
        return FALSE;
    }
    return TRUE;
}

INT HpetInit(VOID) {
    Acpi *AcpiTable;
    HPET *Table;
    UINT64 Cap;
    UINT64 Config;

    if (GHpet.Available) {
        RETURN(SUCCESS);
    }

    AcpiTable = AcpiGet();
    if (!AcpiTable || !AcpiTable->Hpet) {
        RETURN(NOT_FOUND);
    }

    Table = AcpiTable->Hpet;
    if (!HpetValidateTable(Table)) {
        RETURN(INCORRECT_VALUE);
    }

    GHpet.Base = (volatile UINT8 *)(UINTPTR)Table->BaseAddress;
    Cap = MmioRead64(HpetReg64(HPET_REG_CAPABILITIES));
    GHpet.PeriodFs = Cap >> 32;
    if (GHpet.PeriodFs == 0) {
        RETURN(INCORRECT_VALUE);
    }

    GHpet.FrequencyHz = HPET_FS_PER_SEC / GHpet.PeriodFs;

    Config = MmioRead64(HpetReg64(HPET_REG_CONFIG));
    Config |= HPET_CONFIG_ENABLE;
    MmioWrite64(HpetReg64(HPET_REG_CONFIG), Config);

    GHpet.Available = TRUE;

    if (!GHpet.Driver) {
        GHpet.Driver = KDriverGenerateStruct("HPET", DCL1, TRUE, NULLPTR, NULLPTR);
        if (GHpet.Driver) {
            KDriverRegister(GHpet.Driver);
        }
    }

    RETURN(SUCCESS);
}

BOOL HpetIsAvailable(VOID) {
    return GHpet.Available;
}

UINT64 HpetReadCounter(VOID) {
    if (!GHpet.Available) {
        return 0;
    }
    return MmioRead64(HpetReg64(HPET_REG_COUNTER));
}

UINT64 HpetGetFrequency(VOID) {
    return GHpet.FrequencyHz;
}

UINT64 HpetGetPeriod(VOID) {
    return GHpet.PeriodFs;
}

UINT64 HpetGetNanoseconds(VOID) {
    if (!GHpet.Available || GHpet.FrequencyHz == 0) {
        return 0;
    }
    return (HpetReadCounter() * 1000000000ULL) / GHpet.FrequencyHz;
}

UINT64 HpetGetMicroseconds(VOID) {
    if (!GHpet.Available || GHpet.FrequencyHz == 0) {
        return 0;
    }
    return (HpetReadCounter() * 1000000ULL) / GHpet.FrequencyHz;
}

static UINT64 HpetTicksForUs(UINT32 Us) {
    return (GHpet.FrequencyHz * (UINT64)Us) / 1000000ULL;
}

static UINT64 HpetTicksForMs(UINT32 Ms) {
    return (GHpet.FrequencyHz * (UINT64)Ms) / 1000ULL;
}

static VOID HpetWaitTicks(UINT64 Target) {
    while (HpetReadCounter() < Target) {
        asm volatile("pause");
    }
}

VOID HpetDelayUs(UINT32 Us) {
    UINT64 Start;
    UINT64 Target;

    if (!GHpet.Available || Us == 0) {
        return;
    }

    Start = HpetReadCounter();
    Target = Start + HpetTicksForUs(Us);
    HpetWaitTicks(Target);
}

VOID HpetDelayMs(UINT32 Ms) {
    UINT64 Start;
    UINT64 Target;

    if (!GHpet.Available || Ms == 0) {
        return;
    }

    Start = HpetReadCounter();
    Target = Start + HpetTicksForMs(Ms);
    HpetWaitTicks(Target);
}

UINT64 HpetCalibrateTsc(VOID) {
    UINT64 TscStart;
    UINT64 TscEnd;
    UINT64 HpetStart;
    UINT64 HpetEnd;
    const UINT32 CalMs = 10;

    if (!GHpet.Available) {
        return 0;
    }

    HpetStart = HpetReadCounter();
    TscStart = ReadTimeStampCounter();
    HpetWaitTicks(HpetStart + HpetTicksForMs(CalMs));
    TscEnd = ReadTimeStampCounter();
    HpetEnd = HpetReadCounter();

    if (HpetEnd <= HpetStart || TscEnd <= TscStart) {
        return 0;
    }

    return ((TscEnd - TscStart) * GHpet.FrequencyHz) / ((HpetEnd - HpetStart) * 1000ULL);
}

UINT32 HpetCalibrateApicTimer(UINT32 DesiredMs) {
    UINT32 ApicStart;
    UINT32 ApicEnd;
    UINT32 ApicElapsed;
    UINT64 HpetStart;
    UINT64 HpetTarget;

    if (!GHpet.Available || DesiredMs == 0) {
        return 0;
    }

    ApicWriteReg(LAPIC_TIMER_DIV, TIMER_DIV_16);
    ApicWriteReg(LAPIC_TIMER_INITCNT, 0);
    ApicWriteReg(LAPIC_TIMER_INITCNT, 0xFFFFFFFFU);

    ApicStart = ApicReadReg(LAPIC_TIMER_CURRCNT);
    HpetStart = HpetReadCounter();
    HpetTarget = HpetStart + HpetTicksForMs(DesiredMs);
    HpetWaitTicks(HpetTarget);
    ApicEnd = ApicReadReg(LAPIC_TIMER_CURRCNT);
    ApicWriteReg(LAPIC_TIMER_INITCNT, 0);

    if (ApicStart >= ApicEnd) {
        ApicElapsed = ApicStart - ApicEnd;
    } else {
        ApicElapsed = (0xFFFFFFFFU - ApicEnd) + ApicStart + 1;
    }

    if (ApicElapsed < 100 || ApicElapsed >= 0x20000000U) {
        return 0;
    }

    return (ApicElapsed * APIC_TIMER_DIVISOR * 1000U) / DesiredMs;
}
