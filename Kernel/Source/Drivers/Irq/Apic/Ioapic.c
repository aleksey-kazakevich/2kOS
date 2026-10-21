#include <Drivers/Irq/Apic/Ioapic.h>
#include <Drivers/Irq/Apic/Apic.h>
#include <Drivers/Acpi/Acpi.h>
#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <SpinLock.h>
#include <SysDevice.h>
#include <Asm/Mmio.h>
#include <KDriver.h>
#include <Asm/Io.h>
#include <Return.h>

STATIC ListHead GIoapicList;
STATIC UINT32 GIoapicCount = 0;
STATIC IoapicOverride GIoapicOverrides[256];
STATIC SpinLock GIoapicLock;

#define IOAPIC_REG_ID        0x00
#define IOAPIC_REG_VERSION   0x01
#define IOAPIC_REDTBL_BASE   0x10

STATIC INLINE UINT32 IoapicReadReg(volatile VOID *Base, UINT32 Reg) {
    MmioWrite32((volatile UINT32*)((UINTPTR)Base + 0x00), Reg);
    return MmioRead32((volatile UINT32*)((UINTPTR)Base + 0x10));
}

STATIC INLINE VOID IoapicWriteReg(volatile VOID *Base, UINT32 Reg, UINT32 Val) {
    MmioWrite32((volatile UINT32*)((UINTPTR)Base + 0x00), Reg);
    MmioWrite32((volatile UINT32*)((UINTPTR)Base + 0x10), Val);
}

STATIC VOID IoapicSetRedirection(IoapicDevice *Ioapic, UINT32 Index,
                                   UINT32 Low, UINT32 High) {
    UINT32 RegLow = IOAPIC_REDTBL_BASE + Index * 2;
    UINT32 RegHigh = RegLow + 1;
    IoapicWriteReg(Ioapic->VirtAddr, RegHigh, High);
    IoapicWriteReg(Ioapic->VirtAddr, RegLow, Low);
}

STATIC VOID IoapicGetRedirection(IoapicDevice *Ioapic, UINT32 Index,
                                   UINT32 *Low, UINT32 *High) {
    UINT32 RegLow = IOAPIC_REDTBL_BASE + Index * 2;
    UINT32 RegHigh = RegLow + 1;
    *Low = IoapicReadReg(Ioapic->VirtAddr, RegLow);
    *High = IoapicReadReg(Ioapic->VirtAddr, RegHigh);
}

STATIC IoapicDevice* IoapicFindForGsiLocked(UINT32 Gsi) {
    ListHead *Pos;
    IoapicDevice *Best = NULLPTR;

    ListForEach(Pos, &GIoapicList) {
        IoapicDevice *Ioapic = ListEntry(Pos, IoapicDevice, Node);
        if (Gsi >= Ioapic->GsiBase && Gsi < Ioapic->GsiBase + Ioapic->MaxRedir)
            return Ioapic;
        if (Ioapic->GsiBase <= Gsi) {
            if (!Best || Ioapic->GsiBase > Best->GsiBase)
                Best = Ioapic;
        }
    }
    return Best;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

UINT32 IoapicGetCount(VOID) {
    return GIoapicCount;
}

IoapicDevice* IoapicGetDevice(UINT32 Index) {
    UINTPTR F = SpinLockAcquireIrq(&GIoapicLock);

    ListHead *Pos;
    UINT32 Current = 0;
    IoapicDevice *Result = NULLPTR;

    ListForEach(Pos, &GIoapicList) {
        if (Current == Index) {
            Result = ListEntry(Pos, IoapicDevice, Node);
            break;
        }
        Current++;
    }

    SpinLockReleaseIrq(&GIoapicLock, F);
    return Result;
}

IoapicDevice* IoapicFindForGsi(UINT32 Gsi) {
    UINTPTR F = SpinLockAcquireIrq(&GIoapicLock);
    IoapicDevice *Result = IoapicFindForGsiLocked(Gsi);
    SpinLockReleaseIrq(&GIoapicLock, F);
    return Result;
}

INT IoapicInit(VOID) {
    SpinLockInit(&GIoapicLock);

    Acpi *AcpiObj = AcpiGet();
    if (!AcpiObj) RETURN(NO_OBJECT);

    AcpiApicInfo *ApicInfo = &AcpiObj->Apic;
    if (ApicInfo->IoApicCount == 0) RETURN(NO_OBJECT);

    ListInit(&GIoapicList);
    GIoapicCount = 0;

    for (UINT32 I = 0; I < 256; I++) {
        GIoapicOverrides[I].Gsi = 0;
        GIoapicOverrides[I].Flags = 0;
        GIoapicOverrides[I].Valid = FALSE;
    }

    for (UINT32 I = 0; I < ApicInfo->IoApicCount && I < 16; I++) {
        const AcpiIoApic *Info = &ApicInfo->IoApics[I];

        IoapicDevice *Ioapic = (IoapicDevice*)MemoryAllocate(sizeof(IoapicDevice));
        if (!Ioapic) RETURN(NO_MEMORY);

        MemSet(Ioapic, 0, sizeof(IoapicDevice));

        Ioapic->Id = Info->Id;
        Ioapic->Address = Info->Address;
        Ioapic->GsiBase = Info->GsiBase;
        Ioapic->VirtAddr = (volatile VOID*)(UINTPTR)Ioapic->Address;
        Ioapic->Enabled = FALSE;

        IoapicWriteReg(Ioapic->VirtAddr, IOAPIC_REG_VERSION, 0);
        IoWait();
        UINT32 Ver = IoapicReadReg(Ioapic->VirtAddr, IOAPIC_REG_VERSION);
        if (Ver == 0 || Ver == 0xFFFFFFFF) {
            IoWait();
            Ver = IoapicReadReg(Ioapic->VirtAddr, IOAPIC_REG_VERSION);
        }

        if (Ver == 0 || Ver == 0xFFFFFFFF) {
            Ioapic->Version = 0x20;
            Ioapic->MaxRedir = 24;
        } else {
            Ioapic->Version = Ver & 0xFF;
            Ioapic->MaxRedir = ((Ver >> 16) & 0xFF) + 1;
            if (Ioapic->MaxRedir < 24) Ioapic->MaxRedir = 24;
        }

        ListAddTail(&GIoapicList, &Ioapic->Node);
        GIoapicCount++;
    }

    IoapicProcessOverrides();
    IoapicMaskAll();

    KDriverRegister(KDriverGenerateStruct("IOAPIC", 0, TRUE, NULLPTR, NULLPTR));
    IoapicRegisterSysDevices();

    RETURN(SUCCESS);
}

INT IoapicRedirectIrq(UINT32 Gsi, UINT8 Vector, UINT32 ApicId, UINT32 Flags) {
    UINTPTR F = SpinLockAcquireIrq(&GIoapicLock);

    IoapicDevice *Ioapic = IoapicFindForGsiLocked(Gsi);
    if (!Ioapic) {
        SpinLockReleaseIrq(&GIoapicLock, F);
        RETURN(NO_OBJECT);
    }

    UINT32 Index = Gsi - Ioapic->GsiBase;
    if (Index >= Ioapic->MaxRedir) {
        SpinLockReleaseIrq(&GIoapicLock, F);
        RETURN(INCORRECT_VALUE);
    }

    UINT32 Low = Vector & 0xFF;
    Low |= DELIVERY_FIXED;
    if (Flags & IOAPIC_FLAG_ACTIVE_LOW) Low |= IOAPIC_REDIR_POLARITY;
    if (Flags & IOAPIC_FLAG_LEVEL_TRIGGERED) Low |= IOAPIC_REDIR_TRIGGER;
    Low |= IOAPIC_REDIR_MASKED;

    UINT32 High = ApicFormatDestination(ApicId);
    IoapicSetRedirection(Ioapic, Index, Low, High);

    SpinLockReleaseIrq(&GIoapicLock, F);
    RETURN(SUCCESS);
}

INT IoapicUnredirectIrq(UINT32 Gsi) {
    UINTPTR F = SpinLockAcquireIrq(&GIoapicLock);
    IoapicDevice *Ioapic = IoapicFindForGsiLocked(Gsi);
    if (!Ioapic) { SpinLockReleaseIrq(&GIoapicLock, F); RETURN(NO_OBJECT); }

    UINT32 Index = Gsi - Ioapic->GsiBase;
    if (Index >= Ioapic->MaxRedir) { SpinLockReleaseIrq(&GIoapicLock, F); RETURN(INCORRECT_VALUE); }

    UINT32 Low, High;
    IoapicGetRedirection(Ioapic, Index, &Low, &High);
    Low |= IOAPIC_REDIR_MASKED;
    IoapicSetRedirection(Ioapic, Index, Low, High);

    SpinLockReleaseIrq(&GIoapicLock, F);
    RETURN(SUCCESS);
}

VOID IoapicMaskIrq(UINT32 Gsi) {
    UINTPTR F = SpinLockAcquireIrq(&GIoapicLock);
    IoapicDevice *Ioapic = IoapicFindForGsiLocked(Gsi);
    if (Ioapic) {
        UINT32 Index = Gsi - Ioapic->GsiBase;
        if (Index < Ioapic->MaxRedir) {
            UINT32 Low, High;
            IoapicGetRedirection(Ioapic, Index, &Low, &High);
            Low |= IOAPIC_REDIR_MASKED;
            IoapicSetRedirection(Ioapic, Index, Low, High);
        }
    }
    SpinLockReleaseIrq(&GIoapicLock, F);
}

VOID IoapicUnmaskIrq(UINT32 Gsi) {
    UINTPTR F = SpinLockAcquireIrq(&GIoapicLock);
    IoapicDevice *Ioapic = IoapicFindForGsiLocked(Gsi);
    if (Ioapic) {
        UINT32 Index = Gsi - Ioapic->GsiBase;
        if (Index < Ioapic->MaxRedir) {
            UINT32 Low, High;
            IoapicGetRedirection(Ioapic, Index, &Low, &High);
            Low &= ~IOAPIC_REDIR_MASKED;
            IoapicSetRedirection(Ioapic, Index, Low, High);
        }
    }
    SpinLockReleaseIrq(&GIoapicLock, F);
}

VOID IoapicMaskAll(VOID) {
    UINTPTR F = SpinLockAcquireIrq(&GIoapicLock);
    ListHead *Pos;
    ListForEach(Pos, &GIoapicList) {
        IoapicDevice *Ioapic = ListEntry(Pos, IoapicDevice, Node);
        for (UINT32 I = 0; I < Ioapic->MaxRedir; I++) {
            UINT32 Low, High;
            IoapicGetRedirection(Ioapic, I, &Low, &High);
            Low |= IOAPIC_REDIR_MASKED;
            IoapicSetRedirection(Ioapic, I, Low, High);
        }
    }
    SpinLockReleaseIrq(&GIoapicLock, F);
}

VOID IoapicUnmaskAll(VOID) {
    UINTPTR F = SpinLockAcquireIrq(&GIoapicLock);
    ListHead *Pos;
    ListForEach(Pos, &GIoapicList) {
        IoapicDevice *Ioapic = ListEntry(Pos, IoapicDevice, Node);
        for (UINT32 I = 0; I < Ioapic->MaxRedir; I++) {
            UINT32 Low, High;
            IoapicGetRedirection(Ioapic, I, &Low, &High);
            Low &= ~IOAPIC_REDIR_MASKED;
            IoapicSetRedirection(Ioapic, I, Low, High);
        }
    }
    SpinLockReleaseIrq(&GIoapicLock, F);
}

VOID IoapicEoi(UINT32 Gsi) {
    (VOID)Gsi;
    ApicEoi();
}

INT IoapicProcessOverrides(VOID) {
    Acpi *AcpiObj = AcpiGet();
    if (!AcpiObj) RETURN(NO_OBJECT);

    AcpiApicInfo *ApicInfo = &AcpiObj->Apic;

    UINTPTR F = SpinLockAcquireIrq(&GIoapicLock);

    for (UINT32 I = 0; I < 256; I++) {
        GIoapicOverrides[I].Gsi = 0;
        GIoapicOverrides[I].Flags = 0;
        GIoapicOverrides[I].Valid = FALSE;
    }

    for (UINT32 I = 0; I < ApicInfo->IntOverrideCount && I < 16; I++) {
        const AcpiIntOverride *Override = &ApicInfo->IntOverrides[I];
        UINT32 Flags = 0;
        if ((Override->Flags & 0x3) == 0x3) Flags |= IOAPIC_FLAG_ACTIVE_LOW;
        if (((Override->Flags >> 2) & 0x3) == 0x3) Flags |= IOAPIC_FLAG_LEVEL_TRIGGERED;
        if (Override->Source < 256) {
            GIoapicOverrides[Override->Source].Gsi = Override->Gsi;
            GIoapicOverrides[Override->Source].Flags = Flags;
            GIoapicOverrides[Override->Source].Valid = TRUE;
        }
    }

    SpinLockReleaseIrq(&GIoapicLock, F);
    RETURN(SUCCESS);
}

INT IoapicGetOverride(UINT32 Source, UINT32 *Gsi, UINT32 *Flags) {
    if (Source >= 256) RETURN(INCORRECT_VALUE);

    UINTPTR F = SpinLockAcquireIrq(&GIoapicLock);

    INT Rc;
    if (GIoapicOverrides[Source].Valid) {
        if (Gsi) *Gsi = GIoapicOverrides[Source].Gsi;
        if (Flags) *Flags = GIoapicOverrides[Source].Flags;
        Rc = SUCCESS;
    } else {
        if (Gsi) *Gsi = Source;
        if (Flags) *Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
        Rc = SUCCESS;
    }

    SpinLockReleaseIrq(&GIoapicLock, F);
    RETURN(Rc);
}

/* SysDevice — registration only at init, lock is not needed */
STATIC VOID IoapicRegisterOne(IoapicDevice *Ioapic, UINT32 Index,
                               SysDevice *Parent) {
    if (!Ioapic || !Parent) return;

    CHAR Name[SYSDEV_NAME_MAX];
    SnPrintf(Name, sizeof(Name), "IOAPIC%u", Index);

    SysDevice *Dev = SysDeviceCreate(Parent, Name,
                                      "I/O Advanced Programmable Interrupt Controller",
                                      SYSDEV_TYPE_INTERRUPT);
    if (!Dev) return;

    SysDeviceAddResource(Dev, SYSDEV_RES_MMIO, Ioapic->Address, 0x20);
    if (Ioapic->MaxRedir > 0) {
        SysDeviceAddResource(Dev, SYSDEV_RES_IRQ,
                             Ioapic->GsiBase, Ioapic->MaxRedir);
    }

    SysDeviceSetPropU32(Dev, "id", Ioapic->Id);
    SysDeviceSetPropU32(Dev, "version", Ioapic->Version);
    SysDeviceSetPropU32(Dev, "gsi_base", Ioapic->GsiBase);
    SysDeviceSetPropU32(Dev, "max_redir", Ioapic->MaxRedir);
    SysDeviceSetPropU32(Dev, "enabled", Ioapic->Enabled ? 1 : 0);
    SysDeviceSetPropHex(Dev, "phys_addr", Ioapic->Address);
    if (Ioapic->VirtAddr)
        SysDeviceSetPropHex(Dev, "virt_addr", (UINT64)(UINTPTR)Ioapic->VirtAddr);

    KDriver *Drv = KDriverFindByName("IOAPIC");
    if (Drv) SysDeviceBindDriver(Dev, Drv);
    else     SysDeviceSetState(Dev, SYSDEV_STATE_ENABLED);
}

UINT32 IoapicRegisterSysDevices(VOID) {
    if (GIoapicCount == 0) return 0;

    SysDevice *Parent = AcpiGetSysDevice();
    if (!Parent) Parent = SysDeviceGetRoot();
    if (!Parent) return 0;

    UINT32 Count = 0;
    ListHead *Pos;
    UINT32 Index = 0;

    UINTPTR F = SpinLockAcquireIrq(&GIoapicLock);
    ListForEach(Pos, &GIoapicList) {
        IoapicDevice *Ioapic = ListEntry(Pos, IoapicDevice, Node);
        IoapicRegisterOne(Ioapic, Index, Parent);
        Index++;
        Count++;
    }
    SpinLockReleaseIrq(&GIoapicLock, F);

    return Count;
}