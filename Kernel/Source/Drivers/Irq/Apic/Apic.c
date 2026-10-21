#include <Drivers/Irq/Apic/Apic.h>
#include <Limine/LimineParse.h>
#include <Drivers/Acpi/Acpi.h>
#include <Mem/Paging.h>
#include <SpinLock.h>
#include <Asm/Mmio.h>
#include <Asm/Cpu.h>
#include <KDriver.h>
#include <Return.h>

// ============================================================================
// Global State
// ============================================================================

STATIC struct {
    UINT32 Base;
    volatile VOID *Virt;
    volatile BOOL Enabled;
    BOOL X2ApicMode;
    UINT32 Version;
} GApic = {0};

STATIC SpinLock GApicIcrLock;

// ============================================================================
// Internal Helpers
// ============================================================================

STATIC BOOL ApicBaseIsEnabled(VOID) {
    return (ReadMSR(IA32_APIC_BASE_MSR) & APIC_BASE_ENABLE) != 0;
}

STATIC BOOL ApicBaseIsX2Apic(VOID) {
    return (ReadMSR(IA32_APIC_BASE_MSR) & APIC_BASE_X2APIC) != 0;
}

STATIC BOOL ApicTransitionToX2Apic(VOID) {
    UINT64 Base = ReadMSR(IA32_APIC_BASE_MSR);

    if (Base & APIC_BASE_X2APIC) return TRUE;

    if (!(Base & APIC_BASE_ENABLE)) {
        Base |= APIC_BASE_ENABLE;
        WriteMSR(IA32_APIC_BASE_MSR, Base);
    }

    Base = ReadMSR(IA32_APIC_BASE_MSR);
    Base |= APIC_BASE_X2APIC;
    Base |= APIC_BASE_ENABLE;
    WriteMSR(IA32_APIC_BASE_MSR, Base);

    return (ReadMSR(IA32_APIC_BASE_MSR) & APIC_BASE_X2APIC) != 0;
}

STATIC UINT32 ApicOffsetToMsr(UINT32 Reg) {
    return X2APIC_MSR_BASE + (Reg >> 4);
}

STATIC BOOL ApicIsIcrReg(UINT32 Reg) {
    return Reg == LAPIC_ICR_LOW || Reg == LAPIC_ICR_HIGH;
}

STATIC UINT64 ApicReadIcr(VOID) {
    return ReadMSR(X2APIC_MSR_ICR);
}

STATIC VOID ApicWriteIcr(UINT64 Value) {
    WriteMSR(X2APIC_MSR_ICR, Value);
}

STATIC BOOL ApicIcrIsPending(VOID) {
    if (GApic.X2ApicMode) {
        return (ApicReadIcr() & ICR_DELIVERY_PENDING) != 0;
    }
    return (MmioRead32(GApic.Virt + LAPIC_ICR_LOW) & ICR_DELIVERY_PENDING) != 0;
}

STATIC VOID ApicWaitIcr(VOID) {
    INT Timeout = 100000;
    while (ApicIcrIsPending() && Timeout--) {
        CpuPause();
    }
}

// ============================================================================
// Public API
// ============================================================================

BOOL ApicCpuSupportsX2Apic(VOID) {
    UINT32 Eax, Ebx, Ecx, Edx;
    Cpuid(1, &Eax, &Ebx, &Ecx, &Edx);
    return (Ecx & (1 << 21)) != 0;
}

BOOL ApicIsX2ApicMode(VOID) {
    return GApic.X2ApicMode;
}

BOOL ApicIsEnabled(VOID) {
    return GApic.Enabled;
}

UINT32 ApicFormatDestination(UINT32 ApicId) {
    if (GApic.X2ApicMode) return ApicId;
    return ApicId << 24;
}

STATIC INT ApicMapMmio(UINT32 PhysBase) {
    UINT64 VirtBase = 0xFFFFFFFFFEE00000ULL;

    UINT64 *RootTable = PagingGetKernelRoot();
    if (!RootTable) return NO_OBJECT;

    UINT64 Phys = PagingLookupVirt(RootTable, VirtBase);
    if (Phys) {
        GApic.Virt = (volatile VOID*)VirtBase;
        return SUCCESS;
    }

    INT Result = PagingMapPage(RootTable, VirtBase, PhysBase,
                               PTE_PRESENT | PTE_WRITABLE |
                               PTE_CACHE_DISABLE | PTE_WRITE_THROUGH);
    if (Result != SUCCESS) return Result;

    Phys = PagingLookupVirt(RootTable, VirtBase);
    if (!Phys) return INCORRECT_VALUE;

    GApic.Virt = (volatile VOID*)VirtBase;
    return SUCCESS;
}

INT ApicInit(VOID) {
    SpinLockInit(&GApicIcrLock);

    UINT64 BaseMsr = ReadMSR(IA32_APIC_BASE_MSR);

    if (!(BaseMsr & APIC_BASE_ENABLE)) RETURN(NO_OBJECT);

    if (ApicCpuSupportsX2Apic()) {
        if (ApicTransitionToX2Apic()) {
            GApic.X2ApicMode = TRUE;
            GApic.Base = 0;
            GApic.Virt = NULLPTR;
        } else {
            GApic.X2ApicMode = FALSE;
        }
    } else {
        GApic.X2ApicMode = FALSE;
    }

    if (!GApic.X2ApicMode) {
        GApic.Base = (UINT32)(BaseMsr & APIC_BASE_ADDR_MASK);
        INT Result = ApicMapMmio(GApic.Base);
        if (Result != SUCCESS) RETURN(Result);
    }

    UINT32 Ver = ApicReadReg(LAPIC_VERSION);
    if ((Ver & APIC_VERSION_MASK) == 0) RETURN(INCORRECT_VALUE);
    GApic.Version = Ver & APIC_VERSION_MASK;

    KDriverRegister(KDriverGenerateStruct("LAPIC", 0, TRUE, NULLPTR, NULLPTR));
    ApicRegisterSysDevice();

    RETURN(SUCCESS);
}

UINT32 ApicReadReg(UINT32 Reg) {
    if (GApic.X2ApicMode) {
        if (ApicIsIcrReg(Reg)) {
            UINT64 Icr = ApicReadIcr();
            if (Reg == LAPIC_ICR_LOW) return (UINT32)Icr;
            return (UINT32)(Icr >> ICR_DEST_FIELD_SHIFT);
        }
        if (Reg == LAPIC_ID) {
            return (UINT32)ReadMSR(X2APIC_MSR_ID);
        }
        return (UINT32)ReadMSR(ApicOffsetToMsr(Reg));
    }

    if (!GApic.Virt) return 0;
    return MmioRead32(GApic.Virt + Reg);
}

VOID ApicWriteReg(UINT32 Reg, UINT32 Val) {
    if (GApic.X2ApicMode) {
        if (Reg == LAPIC_ICR_LOW) {
            UINT64 Icr = ApicReadIcr();
            Icr = (Icr & (0xFFFFFFFFULL << ICR_DEST_FIELD_SHIFT)) | Val;
            ApicWriteIcr(Icr);
            return;
        }
        if (Reg == LAPIC_ICR_HIGH) {
            UINT64 Icr = ApicReadIcr();
            Icr = (Icr & 0xFFFFFFFFULL) | ((UINT64)Val << ICR_DEST_FIELD_SHIFT);
            ApicWriteIcr(Icr);
            return;
        }
        WriteMSR(ApicOffsetToMsr(Reg), Val);
        return;
    }

    if (!GApic.Virt) return;
    MmioWrite32(GApic.Virt + Reg, Val);
}

VOID ApicEnable(VOID) {
    if (!GApic.Base && !GApic.X2ApicMode) return;

    if (!GApic.X2ApicMode && !ApicBaseIsEnabled()) {
        UINT64 Base = ReadMSR(IA32_APIC_BASE_MSR);
        Base |= APIC_BASE_ENABLE;
        Base = (Base & ~APIC_BASE_ADDR_MASK) | (GApic.Base & APIC_BASE_ADDR_MASK);
        WriteMSR(IA32_APIC_BASE_MSR, Base);
    }

    UINT32 Svr = ApicReadReg(LAPIC_SVR);
    Svr |= (1 << 8);
    Svr &= ~0xFF;
    Svr |= 0xFF;
    ApicWriteReg(LAPIC_SVR, Svr);

    ApicWriteReg(LAPIC_LVT_TIMER, LVT_MASKED);
    ApicWriteReg(LAPIC_TIMER_INITCNT, 0);

    ApicWriteReg(LAPIC_LVT_LINT0, LVT_MASKED);
    ApicWriteReg(LAPIC_LVT_LINT1, LVT_MASKED);

    ApicWriteReg(LAPIC_TPR, 0);

    GApic.Enabled = TRUE;
}

VOID ApicDisable(VOID) {
    if (!GApic.Enabled) return;

    UINT32 Svr = ApicReadReg(LAPIC_SVR);
    Svr &= ~(1 << 8);
    ApicWriteReg(LAPIC_SVR, Svr);

    if (GApic.X2ApicMode || GApic.Base) {
        UINT64 Base = ReadMSR(IA32_APIC_BASE_MSR);
        Base &= ~APIC_BASE_ENABLE;
        WriteMSR(IA32_APIC_BASE_MSR, Base);
    }

    GApic.Enabled = FALSE;
}

VOID ApicEoi(VOID) {
    if (!GApic.Enabled) return;
    ApicWriteReg(LAPIC_EOI, 0);
}

UINT32 ApicGetId(VOID) {
    if (GApic.X2ApicMode) {
        return (UINT32)ReadMSR(X2APIC_MSR_ID);
    }
    UINT32 Id = ApicReadReg(LAPIC_ID);
    return (Id >> 24) & 0xFF;
}

UINT32 ApicGetVersion(VOID) {
    return GApic.Version;
}

// ============================================================================
// IPI — protected by GApicIcrLock (ICR read-modify-write is not atomic for xAPIC)
// ============================================================================

VOID ApicSendIpi(UINT32 ApicId, UINT32 Vector) {
    if (!GApic.Enabled) return;

    UINTPTR F = SpinLockAcquireIrq(&GApicIcrLock);

    if (GApic.X2ApicMode) {
        ApicWaitIcr();
        ApicWriteIcr(((UINT64)ApicId << ICR_DEST_FIELD_SHIFT) |
                     (Vector & 0xFF) | DELIVERY_FIXED | ICR_DEST_PHYSICAL);
    } else {
        ApicWaitIcr();
        ApicWriteReg(LAPIC_ICR_HIGH, ApicId << 24);
        ApicWriteReg(LAPIC_ICR_LOW, Vector | DELIVERY_FIXED | ICR_DEST_PHYSICAL);
    }

    SpinLockReleaseIrq(&GApicIcrLock, F);
}

VOID ApicSendBroadcast(UINT32 Vector) {
    if (!GApic.Enabled) return;

    UINTPTR F = SpinLockAcquireIrq(&GApicIcrLock);

    if (GApic.X2ApicMode) {
        ApicWaitIcr();
        ApicWriteIcr((Vector & 0xFF) | DELIVERY_FIXED | ICR_DEST_ALL);
    } else {
        ApicWaitIcr();
        ApicWriteReg(LAPIC_ICR_LOW, Vector | DELIVERY_FIXED | ICR_DEST_ALL);
    }

    SpinLockReleaseIrq(&GApicIcrLock, F);
}

VOID ApicSendInit(UINT32 ApicId) {
    if (!GApic.Enabled) return;

    UINTPTR F = SpinLockAcquireIrq(&GApicIcrLock);

    if (GApic.X2ApicMode) {
        ApicWaitIcr();
        ApicWriteIcr(((UINT64)ApicId << ICR_DEST_FIELD_SHIFT) |
                     DELIVERY_INIT | ICR_LEVEL_ASSERT | ICR_DEST_PHYSICAL);
        ApicWaitIcr();
        ApicWriteIcr(((UINT64)ApicId << ICR_DEST_FIELD_SHIFT) |
                     DELIVERY_INIT | ICR_LEVEL_DEASSERT | ICR_DEST_PHYSICAL);
        ApicWaitIcr();
    } else {
        ApicWaitIcr();
        ApicWriteReg(LAPIC_ICR_HIGH, ApicId << 24);
        ApicWriteReg(LAPIC_ICR_LOW, DELIVERY_INIT | ICR_LEVEL_ASSERT | ICR_DEST_PHYSICAL);
        ApicWaitIcr();
        ApicWriteReg(LAPIC_ICR_HIGH, ApicId << 24);
        ApicWriteReg(LAPIC_ICR_LOW, DELIVERY_INIT | ICR_LEVEL_DEASSERT | ICR_DEST_PHYSICAL);
        ApicWaitIcr();
    }

    SpinLockReleaseIrq(&GApicIcrLock, F);
}

VOID ApicSendStartup(UINT32 ApicId, UINT32 Vector) {
    if (!GApic.Enabled) return;

    UINTPTR F = SpinLockAcquireIrq(&GApicIcrLock);

    if (GApic.X2ApicMode) {
        ApicWaitIcr();
        ApicWriteIcr(((UINT64)ApicId << ICR_DEST_FIELD_SHIFT) |
                     ((Vector & 0xFF) | DELIVERY_STARTUP | ICR_DEST_PHYSICAL));
        ApicWaitIcr();
    } else {
        ApicWaitIcr();
        ApicWriteReg(LAPIC_ICR_HIGH, ApicId << 24);
        ApicWriteReg(LAPIC_ICR_LOW, (Vector & 0xFF) | DELIVERY_STARTUP | ICR_DEST_PHYSICAL);
        ApicWaitIcr();
    }

    SpinLockReleaseIrq(&GApicIcrLock, F);
}

STATIC const CHAR *ApicGetNodeName(VOID) {
    return GApic.X2ApicMode ? "X2APIC" : "LAPIC";
}

SysDevice *ApicRegisterSysDevice(VOID) {
    const CHAR *Name = ApicGetNodeName();

    SysDevice *Existing = SysDeviceFind(Name);
    if (Existing) return Existing;

    SysDevice *Parent = AcpiGetSysDevice();
    if (!Parent) Parent = SysDeviceGetRoot();
    if (!Parent) return NULLPTR;

    SysDevice *Dev = SysDeviceCreate(Parent,
                                      Name,
                                      GApic.X2ApicMode ?
                                          "Local x2APIC" :
                                          "Local APIC",
                                      SYSDEV_TYPE_INTERRUPT);
    if (!Dev) return NULLPTR;

    if (!GApic.X2ApicMode && GApic.Base) {
        SysDeviceAddResource(Dev, SYSDEV_RES_MMIO, GApic.Base, 0x400);
    }

    // We read the Id fresh, not from globals
    SysDeviceSetPropU32(Dev, "id", ApicGetId());
    SysDeviceSetPropU32(Dev, "version", GApic.Version);
    SysDeviceSetPropU32(Dev, "enabled", GApic.Enabled ? 1 : 0);
    SysDeviceSetPropU32(Dev, "x2apic", GApic.X2ApicMode ? 1 : 0);

    if (GApic.X2ApicMode) {
        SysDeviceSetProp(Dev, "access", "msr");
    } else {
        SysDeviceSetProp(Dev, "access", "mmio");
        if (GApic.Base) SysDeviceSetPropHex(Dev, "mmio_base", GApic.Base);
        if (GApic.Virt) SysDeviceSetPropHex(Dev, "virt_base",
                                            (UINT64)(UINTPTR)GApic.Virt);
    }

    UINT32 AcpiLocalApic = AcpiGetLocalApicAddr();
    if (AcpiLocalApic) {
        SysDeviceSetPropHex(Dev, "acpi_lapic_addr", AcpiLocalApic);
    }

    KDriver *Drv = KDriverFindByName("APIC");
    if (Drv) SysDeviceBindDriver(Dev, Drv);
    else     SysDeviceSetState(Dev, SYSDEV_STATE_ENABLED);

    return Dev;
}