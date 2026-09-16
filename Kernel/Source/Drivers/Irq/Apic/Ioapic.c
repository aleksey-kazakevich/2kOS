#include <Drivers/Irq/Apic/Ioapic.h>
#include <Drivers/Irq/Apic/Apic.h>
#include <Drivers/Acpi/Acpi.h>
#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <SysDevice.h>
#include <Asm/Mmio.h>
#include <KDriver.h>
#include <Asm/Io.h>
#include <Return.h>

// ============================================================================
// Global State
// ============================================================================

STATIC ListHead GIoapicList;
STATIC UINT32 GIoapicCount = 0;
STATIC IoapicOverride GIoapicOverrides[256];

// ============================================================================
// IOAPIC Register Access
// ============================================================================

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

STATIC VOID IoapicSetRedirection(IoapicDevice *Ioapic, UINT32 Index, UINT32 Low, UINT32 High) {
    UINT32 RegLow = IOAPIC_REDTBL_BASE + Index * 2;
    UINT32 RegHigh = RegLow + 1;
    
    IoapicWriteReg(Ioapic->VirtAddr, RegHigh, High);
    IoapicWriteReg(Ioapic->VirtAddr, RegLow, Low);
}

STATIC VOID IoapicGetRedirection(IoapicDevice *Ioapic, UINT32 Index, UINT32 *Low, UINT32 *High) {
    UINT32 RegLow = IOAPIC_REDTBL_BASE + Index * 2;
    UINT32 RegHigh = RegLow + 1;
    
    *Low = IoapicReadReg(Ioapic->VirtAddr, RegLow);
    *High = IoapicReadReg(Ioapic->VirtAddr, RegHigh);
}

// ============================================================================
// Internal Functions
// ============================================================================

STATIC IoapicDevice* IoapicFindForGsiInternal(UINT32 Gsi) {
    ListHead *Pos;
    IoapicDevice *Best = NULLPTR;
    
    ListForEach(Pos, &GIoapicList) {
        IoapicDevice *Ioapic = ListEntry(Pos, IoapicDevice, Node);
        if (Gsi >= Ioapic->GsiBase && Gsi < Ioapic->GsiBase + Ioapic->MaxRedir) {
            return Ioapic;
        }
        if (Ioapic->GsiBase <= Gsi) {
            if (!Best || Ioapic->GsiBase > Best->GsiBase) {
                Best = Ioapic;
            }
        }
    }
    
    return Best;
}

// ============================================================================
// Public API
// ============================================================================

UINT32 IoapicGetCount(VOID) {
    return GIoapicCount;
}

IoapicDevice* IoapicGetDevice(UINT32 Index) {
    ListHead *Pos;
    UINT32 Current = 0;
    
    ListForEach(Pos, &GIoapicList) {
        if (Current == Index) {
            return ListEntry(Pos, IoapicDevice, Node);
        }
        Current++;
    }
    
    return NULLPTR;
}

IoapicDevice* IoapicFindForGsi(UINT32 Gsi) {
    return IoapicFindForGsiInternal(Gsi);
}

INT IoapicInit(VOID) {
    Acpi *AcpiObj = AcpiGet();
    
    if (!AcpiObj) {
        RETURN(NO_OBJECT);
    }
    
    AcpiApicInfo *ApicInfo = &AcpiObj->Apic;
    
    if (ApicInfo->IoApicCount == 0) {
        RETURN(NO_OBJECT);
    }
    
    // Initializing the list
    ListInit(&GIoapicList);
    GIoapicCount = 0;
    
    // Clearing overrides
    for (UINT32 I = 0; I < 256; I++) {
        GIoapicOverrides[I].Gsi = 0;
        GIoapicOverrides[I].Flags = 0;
        GIoapicOverrides[I].Valid = FALSE;
    }
    
    // Creating IOAPIC devices from ACPI data
    for (UINT32 I = 0; I < ApicInfo->IoApicCount && I < 16; I++) {
        const AcpiIoApic *Info = &ApicInfo->IoApics[I];
        
        IoapicDevice *Ioapic = (IoapicDevice*)MemoryAllocate(sizeof(IoapicDevice));
        if (!Ioapic) {
            RETURN(NO_MEMORY);
        }
        
        MemSet(Ioapic, 0, sizeof(IoapicDevice));
        
        Ioapic->Id = Info->Id;
        Ioapic->Address = Info->Address;
        Ioapic->GsiBase = Info->GsiBase;
        Ioapic->VirtAddr = (volatile VOID*)(UINTPTR)Ioapic->Address;
        Ioapic->Enabled = FALSE;
        
        // ============================================================
        // READING THE VERSION FROM THE REGISTER (not from ACPI)
        // ============================================================
        
        // First we write to the selection register
        IoapicWriteReg(Ioapic->VirtAddr, IOAPIC_REG_VERSION, 0);
        IoWait();  // Slight delay
        
        // Reading the version
        UINT32 Ver = IoapicReadReg(Ioapic->VirtAddr, IOAPIC_REG_VERSION);
        
        // If the version is 0 or 0xFFFFFFFF, try again
        if (Ver == 0 || Ver == 0xFFFFFFFF) {
            IoWait();
            Ver = IoapicReadReg(Ioapic->VirtAddr, IOAPIC_REG_VERSION);
        }
        
        // If still 0, set the default values
        if (Ver == 0 || Ver == 0xFFFFFFFF) {
            Ioapic->Version = 0x20;   // Version 32 (common)
            Ioapic->MaxRedir = 24;    // Standard number of entries
        } else {
            Ioapic->Version = Ver & 0xFF;
            Ioapic->MaxRedir = ((Ver >> 16) & 0xFF) + 1;
            
            // If MaxRedir < 24, set the minimum to 24
            if (Ioapic->MaxRedir < 24) {
                Ioapic->MaxRedir = 24;
            }
        }
        
        ListAddTail(&GIoapicList, &Ioapic->Node);
        GIoapicCount++;
    }
    
    // Handling overrides
    IoapicProcessOverrides();
    
    // Masking all IRQs
    IoapicMaskAll();
    
    // Registering the driver
    KDriverRegister(KDriverGenerateStruct("IOAPIC", 0, TRUE, NULLPTR, NULLPTR));

    IoapicRegisterSysDevices();
    
    RETURN(SUCCESS);
}

INT IoapicRedirectIrq(UINT32 Gsi, UINT8 Vector, UINT32 ApicId, UINT32 Flags) {
    IoapicDevice *Ioapic = IoapicFindForGsiInternal(Gsi);
    if (!Ioapic) RETURN(NO_OBJECT);
    
    UINT32 Index = Gsi - Ioapic->GsiBase;
    if (Index >= Ioapic->MaxRedir) RETURN(INCORRECT_VALUE);
    
    UINT32 Low = Vector & 0xFF;
    Low |= DELIVERY_FIXED;
    
    if (Flags & IOAPIC_FLAG_ACTIVE_LOW) {
        Low |= IOAPIC_REDIR_POLARITY;
    }
    
    if (Flags & IOAPIC_FLAG_LEVEL_TRIGGERED) {
        Low |= IOAPIC_REDIR_TRIGGER;
    }
    
    // Let's start with the disguised one
    Low |= IOAPIC_REDIR_MASKED;
    
    UINT32 High = ApicFormatDestination(ApicId);
    
    IoapicSetRedirection(Ioapic, Index, Low, High);
    
    RETURN(SUCCESS);
}

INT IoapicUnredirectIrq(UINT32 Gsi) {
    IoapicDevice *Ioapic = IoapicFindForGsiInternal(Gsi);
    if (!Ioapic) RETURN(NO_OBJECT);
    
    UINT32 Index = Gsi - Ioapic->GsiBase;
    if (Index >= Ioapic->MaxRedir) RETURN(INCORRECT_VALUE);
    
    UINT32 Low, High;
    IoapicGetRedirection(Ioapic, Index, &Low, &High);
    Low |= IOAPIC_REDIR_MASKED;
    IoapicSetRedirection(Ioapic, Index, Low, High);
    
    RETURN(SUCCESS);
}

VOID IoapicMaskIrq(UINT32 Gsi) {
    IoapicDevice *Ioapic = IoapicFindForGsiInternal(Gsi);
    if (!Ioapic) return;
    
    UINT32 Index = Gsi - Ioapic->GsiBase;
    if (Index >= Ioapic->MaxRedir) return;
    
    UINT32 Low, High;
    IoapicGetRedirection(Ioapic, Index, &Low, &High);
    Low |= IOAPIC_REDIR_MASKED;
    IoapicSetRedirection(Ioapic, Index, Low, High);
}

VOID IoapicUnmaskIrq(UINT32 Gsi) {
    IoapicDevice *Ioapic = IoapicFindForGsiInternal(Gsi);
    if (!Ioapic) return;
    
    UINT32 Index = Gsi - Ioapic->GsiBase;
    if (Index >= Ioapic->MaxRedir) return;
    
    UINT32 Low, High;
    IoapicGetRedirection(Ioapic, Index, &Low, &High);
    Low &= ~IOAPIC_REDIR_MASKED;
    IoapicSetRedirection(Ioapic, Index, Low, High);
}

VOID IoapicMaskAll(VOID) {
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
}

VOID IoapicUnmaskAll(VOID) {
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
}

VOID IoapicEoi(UINT32 Gsi) {
    (VOID)Gsi;
    ApicEoi();
}

INT IoapicProcessOverrides(VOID) {
    Acpi *AcpiObj = AcpiGet();
    if (!AcpiObj) RETURN(NO_OBJECT);
    
    AcpiApicInfo *ApicInfo = &AcpiObj->Apic;
    
    // Clearing existing overrides
    for (UINT32 I = 0; I < 256; I++) {
        GIoapicOverrides[I].Gsi = 0;
        GIoapicOverrides[I].Flags = 0;
        GIoapicOverrides[I].Valid = FALSE;
    }
    
    // Filling from ACPI
    for (UINT32 I = 0; I < ApicInfo->IntOverrideCount && I < 16; I++) {
        const AcpiIntOverride *Override = &ApicInfo->IntOverrides[I];
        
        UINT32 Flags = 0;
        
        if ((Override->Flags & 0x3) == 0x3) {
            Flags |= IOAPIC_FLAG_ACTIVE_LOW;
        }
        
        if (((Override->Flags >> 2) & 0x3) == 0x3) {
            Flags |= IOAPIC_FLAG_LEVEL_TRIGGERED;
        }
        
        if (Override->Source < 256) {
            GIoapicOverrides[Override->Source].Gsi = Override->Gsi;
            GIoapicOverrides[Override->Source].Flags = Flags;
            GIoapicOverrides[Override->Source].Valid = TRUE;
        }
    }
    
    RETURN(SUCCESS);
}

INT IoapicGetOverride(UINT32 Source, UINT32 *Gsi, UINT32 *Flags) {
    if (Source >= 256) RETURN(INCORRECT_VALUE);
    
    if (GIoapicOverrides[Source].Valid) {
        if (Gsi) *Gsi = GIoapicOverrides[Source].Gsi;
        if (Flags) *Flags = GIoapicOverrides[Source].Flags;
        RETURN(SUCCESS);
    }
    
    // No overriding - use the standard one
    if (Gsi) *Gsi = Source;
    if (Flags) *Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
    RETURN(SUCCESS);
}

STATIC VOID IoapicRegisterOne(IoapicDevice *Ioapic, UINT32 Index,
                               SysDevice *Parent) {
    if (!Ioapic || !Parent) return;

    CHAR Name[SYSDEV_NAME_MAX];
    SnPrintf(Name, sizeof(Name), "IOAPIC%u", Index);

    SysDevice *Dev = SysDeviceCreate(Parent,
                                      Name,
                                      "I/O Advanced Programmable Interrupt Controller",
                                      SYSDEV_TYPE_INTERRUPT);
    if (!Dev) return;

    // --- Ресурсы ---

    // MMIO регион (типично 0x20 байт: IOREGSEL + IOWIN + padding)
    SysDeviceAddResource(Dev, SYSDEV_RES_MMIO, Ioapic->Address, 0x20);

    // GSI range — это тоже «ресурс» устройства
    // (диапазон прерываний, которые оно обслуживает)
    if (Ioapic->MaxRedir > 0) {
        SysDeviceAddResource(Dev, SYSDEV_RES_IRQ,
                             Ioapic->GsiBase, Ioapic->MaxRedir);
    }

    // --- Свойства ---

    SysDeviceSetPropU32(Dev, "id", Ioapic->Id);
    SysDeviceSetPropU32(Dev, "version", Ioapic->Version);
    SysDeviceSetPropU32(Dev, "gsi_base", Ioapic->GsiBase);
    SysDeviceSetPropU32(Dev, "max_redir", Ioapic->MaxRedir);
    SysDeviceSetPropU32(Dev, "enabled", Ioapic->Enabled ? 1 : 0);
    SysDeviceSetPropHex(Dev, "phys_addr", Ioapic->Address);

    if (Ioapic->VirtAddr) {
        SysDeviceSetPropHex(Dev, "virt_addr",
                            (UINT64)(UINTPTR)Ioapic->VirtAddr);
    }

    // --- Привязка к KDriver ---

    KDriver *Drv = KDriverFindByName("IOAPIC");
    if (Drv) {
        SysDeviceBindDriver(Dev, Drv);
    } else {
        SysDeviceSetState(Dev, SYSDEV_STATE_ENABLED);
    }
}

UINT32 IoapicRegisterSysDevices(VOID) {
    if (GIoapicCount == 0) return 0;

    // Родитель — ACPI bus, если есть. Иначе — root.
    SysDevice *Parent = AcpiGetSysDevice();
    if (!Parent) Parent = SysDeviceGetRoot();
    if (!Parent) return 0;

    UINT32 Count = 0;
    ListHead *Pos;
    UINT32 Index = 0;

    ListForEach(Pos, &GIoapicList) {
        IoapicDevice *Ioapic = ListEntry(Pos, IoapicDevice, Node);
        IoapicRegisterOne(Ioapic, Index, Parent);
        Index++;
        Count++;
    }

    return Count;
}