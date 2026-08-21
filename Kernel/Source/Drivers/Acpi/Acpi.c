#include <Drivers/Acpi/Acpi.h>
#include <Lib/String.h>
#include <Asm/Io.h>
#include <Asm/Cpu.h>
#include <KDriver.h>
#include <Return.h>
#include <Mem/Allocator.h>
#include <Mem/Paging.h>

// ============================================================================
// Global State
// ============================================================================

static Acpi GAcpi = {0};

// ============================================================================
// Internal Helpers
// ============================================================================

UINT8 AcpiChecksum(VOID *Table, UINT32 Length) {
    UINT8 Sum = 0;
    UINT8 *Bytes = (UINT8*)Table;
    for (UINT32 I = 0; I < Length; I++) {
        Sum += Bytes[I];
    }
    return Sum;
}

static BOOL AcpiValidateRsdp(RSDPV2 *Rsdp) {
    if (MemCmp(Rsdp->V1.Signature, ACPI_RSDP_SIGNATURE, 8) != 0) {
        return FALSE;
    }
    
    UINT8 V1Checksum = AcpiChecksum(Rsdp, ACPI_RSDP_V1_SIZE);
    if (V1Checksum != 0) {
        return FALSE;
    }
    
    if (Rsdp->V1.Revision >= 2) {
        if (Rsdp->Length > sizeof(RSDPV2)) {
            return FALSE;
        }
        UINT8 V2Checksum = AcpiChecksum(Rsdp, Rsdp->Length);
        if (V2Checksum != 0) {
            return FALSE;
        }
        GAcpi.UseXsdt = TRUE;
    }
    
    return TRUE;
}

static SDTHeader* AcpiFindTableInternal(const CHAR *Signature, INT Index) {
    if (!GAcpi.Rsdt && !GAcpi.Xsdt) return NULLPTR;
    
    INT Found = 0;
    
    if (GAcpi.UseXsdt && GAcpi.Xsdt) {
        UINT32 Count = (GAcpi.Xsdt->Header.Length - sizeof(SDTHeader)) / 8;
        UINT64 *Entries = (UINT64*)((UINTPTR)GAcpi.Xsdt + sizeof(SDTHeader));
        
        for (UINT32 I = 0; I < Count; I++) {
            SDTHeader *H = (SDTHeader*)(UINTPTR)Entries[I];
            if (MemCmp(H->Signature, Signature, 4) == 0) {
                if (Found == Index) {
                    if (AcpiChecksum(H, H->Length) == 0) {
                        return H;
                    }
                }
                Found++;
            }
        }
    } else if (GAcpi.Rsdt) {
        UINT32 Count = (GAcpi.Rsdt->Header.Length - sizeof(SDTHeader)) / 4;
        UINT32 *Entries = (UINT32*)((UINTPTR)GAcpi.Rsdt + sizeof(SDTHeader));
        
        for (UINT32 I = 0; I < Count; I++) {
            SDTHeader *H = (SDTHeader*)(UINTPTR)Entries[I];
            if (MemCmp(H->Signature, Signature, 4) == 0) {
                if (Found == Index) {
                    if (AcpiChecksum(H, H->Length) == 0) {
                        return H;
                    }
                }
                Found++;
            }
        }
    }
    
    return NULLPTR;
}

// ============================================================================
// MADT Parser (now here!)
// ============================================================================

static INT AcpiParseMadt(VOID) {
    if (!GAcpi.Madt) RETURN(NO_OBJECT);
    
    AcpiApicInfo *Apic = &GAcpi.Apic;
    MemSet(Apic, 0, sizeof(AcpiApicInfo));
    
    Apic->LocalApicAddress = GAcpi.Madt->LocalApicAddress;
    
    UINT8 *Entry = (UINT8*)GAcpi.Madt + sizeof(MADT);
    UINT8 *End = (UINT8*)GAcpi.Madt + GAcpi.Madt->Header.Length;
    
    while (Entry < End) {
        MADTEntryHeader *Header = (MADTEntryHeader*)Entry;
        
        switch (Header->Type) {
            case MADT_TYPE_LOCAL_APIC: {
                MADTLocalApic *Lapic = (MADTLocalApic*)Entry;
                if (Apic->ProcessorCount < 64) {
                    Apic->Processors[Apic->ProcessorCount].AcpiProcessorUid = Lapic->AcpiProcessorId;
                    Apic->Processors[Apic->ProcessorCount].ApicId = Lapic->ApicId;
                    Apic->Processors[Apic->ProcessorCount].Enabled = (Lapic->Flags & 1) != 0;
                    Apic->Processors[Apic->ProcessorCount].X2Apic = FALSE;
                    Apic->ProcessorCount++;
                }
                break;
            }
            
            case MADT_TYPE_PROCESSOR_LOCAL_X2APIC: {
                MADTLocalX2Apic *X2Apic = (MADTLocalX2Apic*)Entry;
                if (Apic->ProcessorCount < 64) {
                    Apic->Processors[Apic->ProcessorCount].AcpiProcessorUid = X2Apic->AcpiProcessorUid;
                    Apic->Processors[Apic->ProcessorCount].ApicId = X2Apic->LocalX2ApicId;
                    Apic->Processors[Apic->ProcessorCount].Enabled = (X2Apic->Flags & 1) != 0;
                    Apic->Processors[Apic->ProcessorCount].X2Apic = TRUE;
                    Apic->UsesX2Apic = TRUE;
                    Apic->ProcessorCount++;
                }
                break;
            }
            
            case MADT_TYPE_IO_APIC: {
                MADTIoApic *Ioapic = (MADTIoApic*)Entry;
                if (Apic->IoApicCount < 16) {
                    Apic->IoApics[Apic->IoApicCount].Address = Ioapic->IoApicAddress;
                    Apic->IoApics[Apic->IoApicCount].GsiBase = Ioapic->GlobalSystemInterruptBase;
                    Apic->IoApics[Apic->IoApicCount].Id = Ioapic->IoApicId;
                    // Version and MaxRedir will be filled when IOAPIC is initialized
                    Apic->IoApics[Apic->IoApicCount].Version = 0;
                    Apic->IoApics[Apic->IoApicCount].MaxRedir = 0;
                    Apic->IoApicCount++;
                }
                break;
            }
            
            case MADT_TYPE_INT_SOURCE_OVERRIDE: {
                MADTIntSourceOverride *Override = (MADTIntSourceOverride*)Entry;
                if (Apic->IntOverrideCount < 16) {
                    Apic->IntOverrides[Apic->IntOverrideCount].Bus = Override->Bus;
                    Apic->IntOverrides[Apic->IntOverrideCount].Source = Override->Source;
                    Apic->IntOverrides[Apic->IntOverrideCount].Gsi = Override->GlobalSystemInterrupt;
                    Apic->IntOverrides[Apic->IntOverrideCount].Flags = Override->Flags;
                    Apic->IntOverrideCount++;
                }
                break;
            }
        }
        
        Entry += Header->Length;
    }
    
    RETURN(SUCCESS);
}

// ============================================================================
// Public API
// ============================================================================

Acpi *AcpiGet(VOID) {
    return &GAcpi;
}

SDTHeader* AcpiFindTable(const CHAR *Signature) {
    return AcpiFindTableInternal(Signature, 0);
}

SDTHeader* AcpiFindTableWithIndex(const CHAR *Signature, INT Index) {
    return AcpiFindTableInternal(Signature, Index);
}

AcpiApicInfo* AcpiGetApicInfo(VOID) {
    return &GAcpi.Apic;
}

UINT32 AcpiGetLocalApicAddr(VOID) {
    return GAcpi.Apic.LocalApicAddress;
}

UINT32 AcpiGetProcessorCount(VOID) {
    return GAcpi.Apic.ProcessorCount;
}

const AcpiProcessor* AcpiGetProcessor(UINT32 Index) {
    if (Index >= GAcpi.Apic.ProcessorCount) return NULLPTR;
    return &GAcpi.Apic.Processors[Index];
}

UINT32 AcpiGetIoApicCount(VOID) {
    return GAcpi.Apic.IoApicCount;
}

const AcpiIoApic* AcpiGetIoApic(UINT32 Index) {
    if (Index >= GAcpi.Apic.IoApicCount) return NULLPTR;
    return &GAcpi.Apic.IoApics[Index];
}

UINT32 AcpiGetOverrideCount(VOID) {
    return GAcpi.Apic.IntOverrideCount;
}

const AcpiIntOverride* AcpiGetOverride(UINT32 Index) {
    if (Index >= GAcpi.Apic.IntOverrideCount) return NULLPTR;
    return &GAcpi.Apic.IntOverrides[Index];
}

INT AcpiGetOverrideForSource(UINT8 Source, AcpiIntOverride *Out) {
    if (!Out) RETURN(INCORRECT_VALUE);
    
    for (UINT32 I = 0; I < GAcpi.Apic.IntOverrideCount; I++) {
        if (GAcpi.Apic.IntOverrides[I].Source == Source) {
            *Out = GAcpi.Apic.IntOverrides[I];
            RETURN(SUCCESS);
        }
    }
    
    RETURN(NOT_FOUND);
}

INT AcpiInit(UINT64 RsdpAddr) {
    if (!RsdpAddr) RETURN(NO_OBJECT);
    
    MemSet(&GAcpi, 0, sizeof(GAcpi));
    GAcpi.Rsdp = (RSDPV2*)(UINTPTR)RsdpAddr;
    
    if (!AcpiValidateRsdp(GAcpi.Rsdp)) {
        RETURN(NO_OBJECT);
    }
    
    // We get RSDT/XSDT
    if (GAcpi.UseXsdt) {
        GAcpi.Xsdt = (XSDT*)(UINTPTR)GAcpi.Rsdp->XsdtAddress;
        if (AcpiChecksum(GAcpi.Xsdt, GAcpi.Xsdt->Header.Length) != 0) {
            RETURN(INCORRECT_VALUE);
        }
    } else {
        GAcpi.Rsdt = (RSDT*)(UINTPTR)GAcpi.Rsdp->V1.RsdtAddress;
        if (AcpiChecksum(GAcpi.Rsdt, GAcpi.Rsdt->Header.Length) != 0) {
            RETURN(INCORRECT_VALUE);
        }
    }
    
    // Finding the base tables
    GAcpi.Fadt = (FADT*)AcpiFindTable("FACP");
    GAcpi.Madt = (MADT*)AcpiFindTable("APIC");
    GAcpi.Hpet = (HPET*)AcpiFindTable("HPET");
    GAcpi.Mcfg = (MCFG*)AcpiFindTable("MCFG");
    
    // Parse DSDT
    if (GAcpi.Fadt) {
        if (GAcpi.UseXsdt && GAcpi.Fadt->XDsdt) {
            GAcpi.Dsdt = (SDTHeader*)(UINTPTR)GAcpi.Fadt->XDsdt;
        } else if (GAcpi.Fadt->Dsdt) {
            GAcpi.Dsdt = (SDTHeader*)(UINTPTR)GAcpi.Fadt->Dsdt;
        }
    }
    
    // Collecting SSDT
    GAcpi.SsdtCount = 0;
    if (GAcpi.UseXsdt && GAcpi.Xsdt) {
        UINT32 Count = (GAcpi.Xsdt->Header.Length - sizeof(SDTHeader)) / 8;
        UINT64 *Entries = (UINT64*)((UINTPTR)GAcpi.Xsdt + sizeof(SDTHeader));
        for (UINT32 I = 0; I < Count && GAcpi.SsdtCount < 16; I++) {
            SDTHeader *Table = (SDTHeader*)(UINTPTR)Entries[I];
            if (MemCmp(Table->Signature, "SSDT", 4) == 0) {
                GAcpi.Ssdts[GAcpi.SsdtCount++] = Table;
            }
        }
    } else if (GAcpi.Rsdt) {
        UINT32 Count = (GAcpi.Rsdt->Header.Length - sizeof(SDTHeader)) / 4;
        UINT32 *Entries = (UINT32*)((UINTPTR)GAcpi.Rsdt + sizeof(SDTHeader));
        for (UINT32 I = 0; I < Count && GAcpi.SsdtCount < 16; I++) {
            SDTHeader *Table = (SDTHeader*)(UINTPTR)Entries[I];
            if (MemCmp(Table->Signature, "SSDT", 4) == 0) {
                GAcpi.Ssdts[GAcpi.SsdtCount++] = Table;
            }
        }
    }
    
    // Getting APIC Bus Frequency
    if (GAcpi.Fadt && GAcpi.Fadt->Header.Revision >= 3) {
        GAcpi.ApicBusFreq = GAcpi.Fadt->ApicBusFreq;
        if (GAcpi.ApicBusFreq < 100000000) {
            GAcpi.ApicBusFreq = 0;
        }
    } else {
        GAcpi.ApicBusFreq = 0;
    }
    
    // Parse MADT (HERE, not in the APIC driver!)
    if (GAcpi.Madt) {
        AcpiParseMadt();
    }
    
    // Registering the ACPI driver
    KDriverRegister(KDriverGenerateStruct("ACPI", 0, TRUE, &GAcpi, NULLPTR));
    
    RETURN(SUCCESS);
}

// ============================================================================
// Power Management
// ============================================================================

VOID AcpiReboot(VOID) {
    if (!GAcpi.Fadt) {
        Outb(0x64, 0xFE);
        return;
    }
    
    // Trying via ACPI Reset Register
    UINT8 *ResetReg = GAcpi.Fadt->ResetReg;
    
    if (ResetReg[0] == 0x01) { // System I/O
        UINT16 Port = *(UINT16*)(ResetReg + 4);
        UINT8 Value = GAcpi.Fadt->ResetValue;
        Outb(Port, Value);
    } else if (ResetReg[0] == 0x02) { // Memory-mapped
        UINT64 PhysAddr = *(UINT64*)(ResetReg + 4);
        UINT8 Value = GAcpi.Fadt->ResetValue;
        UINT64 VirtAddr = PhysToVirt(PhysAddr);
        *(volatile UINT8*)(UINTPTR)VirtAddr = Value;
    } else {
        // Fallback: keyboard controller reset
        Outb(0x64, 0xFE);
    }
    
    while(1) {
        Halt();
    }
}

VOID AcpiShutdown(VOID) {
    if (!GAcpi.Fadt) {
        return;
    }
    
    UINT16 Pm1aPort = GAcpi.Fadt->Pm1aCntBlk;
    if (!Pm1aPort) {
        return;
    }
    
    // S5 sleep state (shutdown)
    UINT16 Value = (5 << 10) | (1 << 13);
    Outw(Pm1aPort, Value);
    
    UINT16 Pm1bPort = GAcpi.Fadt->Pm1bCntBlk;
    if (Pm1bPort) {
        Outw(Pm1bPort, Value);
    }
    
    LocalInterruptsDisable();

    while(1) {
        Halt();
    }
}
