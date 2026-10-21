#include <Drivers/Irq/Apic/ApicRegs.h>
#include <Drivers/Acpi/AcpiTables.h>
#include <Drivers/Irq/Apic/Apic.h>
#include <Limine/LimineParse.h>
#include <Drivers/Acpi/Acpi.h>
#include <Drivers/Hpet.h>
#include <Lib/String.h>
#include <Mem/Paging.h>
#include <Asm/Mmio.h>
#include <Asm/Cpu.h>
#include <KDriver.h>
#include <Return.h>

#define HPET_FS_PER_SEC         1000000000000000ULL
#define APIC_TIMER_DIVISOR      16

STATIC struct {
    volatile UINT8 *Base;
    UINT64 FrequencyHz;
    UINT64 PeriodFs;
    BOOL Is64Bit;
    BOOL Available;
    KDriver *Driver;
} GHpet = {0};

STATIC volatile UINT64 *HpetReg64(UINT32 Offset) {
    return (volatile UINT64 *)(GHpet.Base + Offset);
}

STATIC BOOL HpetValidateTable(HPET *Table) {
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
    if (!AcpiTable) {
        RETURN(NO_OBJECT);
    }
    if (!AcpiTable->Hpet) {
        RETURN(NOT_FOUND);
    }

    Table = AcpiTable->Hpet;

    if (!HpetValidateTable(Table)) {
        RETURN(INCORRECT_VALUE);
    }

    UINT64 Hhdm = LimineGetHHDMOffset();
    UINT64 PhysBase = Table->BaseAddress;
    UINT64 VirtBase = PhysToVirt(PhysBase);
    GHpet.Base = (volatile UINT8 *)VirtBase;

    Cap = MmioRead64(HpetReg64(HPET_REG_CAPABILITIES));

    GHpet.PeriodFs = Cap >> 32;
    GHpet.Is64Bit = (Cap >> 13) & 1;

    if (GHpet.PeriodFs == 0) {
        RETURN(INCORRECT_VALUE);
    }

    if (GHpet.PeriodFs > 100000000ULL) {
        // Typical HPET period: 1..100 ns in femtoseconds → 1e6..1e8
        RETURN(INCORRECT_VALUE);
    }

    GHpet.FrequencyHz = HPET_FS_PER_SEC / GHpet.PeriodFs;

    Config = MmioRead64(HpetReg64(HPET_REG_CONFIG));

    Config |= HPET_CONFIG_ENABLE;
    MmioWrite64(HpetReg64(HPET_REG_CONFIG), Config);

    Config = MmioRead64(HpetReg64(HPET_REG_CONFIG));

    if (!(Config & HPET_CONFIG_ENABLE)) {
        RETURN(DEVICE_ERROR);
    }

    // We check that the counter is actually incremented.
    UINT64 C1 = HpetReadCounter();
    for (volatile INT I = 0; I < 1000000; I++) {
        asm volatile("pause");
    }
    UINT64 C2 = HpetReadCounter();

    if (C2 == C1) {
        RETURN(DEVICE_ERROR);
    }

    GHpet.Available = TRUE;

    if (!GHpet.Driver) {
        GHpet.Driver = KDriverGenerateStruct("HPET", DCL1, TRUE, NULLPTR, NULLPTR);
        if (GHpet.Driver) {
            KDriverRegister(GHpet.Driver);
        }
    }

    HpetRegisterSysDevice();

    RETURN(SUCCESS);
}

BOOL HpetIsAvailable(VOID) {
    return GHpet.Available;
}

UINT64 HpetReadCounter(VOID) {
    if (!GHpet.Base) {
        return 0;
    }
    volatile UINT64 *Reg = HpetReg64(HPET_REG_COUNTER);
    UINT64 Val = *Reg;
    if (!GHpet.Is64Bit) {
        Val &= 0xFFFFFFFFULL;
    }
    return Val;
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

UINT64 HpetTicksForUs(UINT32 Us) {
    return (GHpet.FrequencyHz * (UINT64)Us) / 1000000ULL;
}

UINT64 HpetTicksForMs(UINT32 Ms) {
    return (GHpet.FrequencyHz * (UINT64)Ms) / 1000ULL;
}

STATIC VOID HpetWaitTicks(UINT64 Target) {
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

SysDevice *HpetRegisterSysDevice(VOID) {
    if (!GHpet.Available) return NULLPTR;

    // We check that HPET has not been registered yet.
    SysDevice *Existing = SysDeviceFind("HPET");
    if (Existing) return Existing;

    //The parent is ACPI bus, if there is one. Otherwise— it's root.
    SysDevice *Parent = AcpiGetSysDevice();
    if (!Parent) Parent = SysDeviceGetRoot();
    if (!Parent) return NULLPTR;

    SysDevice *Dev = SysDeviceCreate(Parent,
                                      "HPET",
                                      "High Precision Event Timer",
                                      SYSDEV_TYPE_TIMER);
    if (!Dev) return NULLPTR;

    // Resources

    // MMIO region (usually 1024 bytes)
    if (GHpet.Base) {
        UINT64 HpetPhys = VirtToPhys((UINT64)(UINTPTR)GHpet.Base);
        SysDeviceAddResource(Dev, SYSDEV_RES_MMIO, HpetPhys, 1024);
    }

    // Properties

    SysDeviceSetPropU64(Dev, "freq_hz", GHpet.FrequencyHz);
    SysDeviceSetPropU64(Dev, "period_fs", GHpet.PeriodFs);
    SysDeviceSetPropU32(Dev, "is_64bit", GHpet.Is64Bit ? 1 : 0);
    SysDeviceSetPropU32(Dev, "comparators", 0);   // we will find out from Capabilities if desired.

    if (GHpet.Base) {
        SysDeviceSetPropHex(Dev, "phys_addr",
                            VirtToPhys((UINT64)(UINTPTR)GHpet.Base));
        SysDeviceSetPropHex(Dev, "virt_addr",
                            (UINT64)(UINTPTR)GHpet.Base);
    }

    // We calculate how many ns one tick lasts (the inverse of the frequency)
    if (GHpet.FrequencyHz) {
        SysDeviceSetPropU64(Dev, "tick_ns",
                            1000000000ULL / GHpet.FrequencyHz);
    }

    // Linking to KDriver

    if (GHpet.Driver) {
        SysDeviceBindDriver(Dev, GHpet.Driver);
    } else {
        SysDeviceSetState(Dev, SYSDEV_STATE_ENABLED);
    }

    return Dev;
}