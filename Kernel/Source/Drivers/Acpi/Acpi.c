#include <Drivers/Acpi/Acpi.h>
#include <Drivers/Acpi/Aml.h>
#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Mem/Paging.h>
#include <SysDevice.h>
#include <Lib/StdIo.h>
#include <Asm/Cpu.h>
#include <KDriver.h>
#include <Asm/Io.h>
#include <Return.h>

// ============================================================================
// Global State
// ============================================================================

STATIC Acpi GAcpi = {0};
STATIC SysDevice *GAcpiDevice = NULLPTR;

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

STATIC BOOL AcpiValidateRsdp(RSDPV2 *Rsdp) {
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

STATIC SDTHeader* AcpiFindTableInternal(const CHAR *Signature, INT Index) {
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
// MADT Parser
// ============================================================================

STATIC INT AcpiParseMadt(VOID) {
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
    
    GAcpi.Fadt = (FADT*)AcpiFindTable("FACP");
    GAcpi.Madt = (MADT*)AcpiFindTable("APIC");
    GAcpi.Hpet = (HPET*)AcpiFindTable("HPET");
    GAcpi.Mcfg = (MCFG*)AcpiFindTable("MCFG");
    
    if (GAcpi.Fadt) {
        if (GAcpi.UseXsdt && GAcpi.Fadt->XDsdt) {
            GAcpi.Dsdt = (SDTHeader*)(UINTPTR)GAcpi.Fadt->XDsdt;
        } else if (GAcpi.Fadt->Dsdt) {
            GAcpi.Dsdt = (SDTHeader*)(UINTPTR)GAcpi.Fadt->Dsdt;
        }
    }
    
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
    
    if (GAcpi.Fadt && GAcpi.Fadt->Header.Revision >= 3) {
        GAcpi.ApicBusFreq = GAcpi.Fadt->ApicBusFreq;
        if (GAcpi.ApicBusFreq < 100000000) {
            GAcpi.ApicBusFreq = 0;
        }
    } else {
        GAcpi.ApicBusFreq = 0;
    }
    
    if (GAcpi.Madt) {
        AcpiParseMadt();
    }
	
	AmlInit(GAcpi.Dsdt, GAcpi.Ssdts, GAcpi.SsdtCount);
    
    KDriverRegister(KDriverGenerateStruct("ACPI", 0, TRUE, &GAcpi, NULLPTR));

    AcpiRegisterSysDevices();
    
    RETURN(SUCCESS);
}

// Auxiliary function for the Generic Address Structure
STATIC UINT64 AcpiReadGas(UINT8 *Gas, UINT8 *WidthOut) {
    if (!Gas || Gas[0] == 0) return 0;
    
    UINT8 SpaceId = Gas[0];
    UINT8 BitWidth = Gas[1];
    UINT64 Addr = *(UINT64*)(Gas + 4);
    
    if (WidthOut) *WidthOut = BitWidth;
    
    // We only support System Memory and System I/O
    if (SpaceId != 0 && SpaceId != 1) return 0;
    
    return Addr;
}

STATIC UINT8 AcpiGetGasWidth(UINT8 *Gas) {
    if (!Gas || Gas[0] == 0) return 0;
    return Gas[1];
}

STATIC UINT8 AcpiGetGasSpaceId(UINT8 *Gas) {
    if (!Gas || Gas[0] == 0) return 0xFF;
    return Gas[0];
}

// Writing to the ACPI register via GAS
STATIC VOID AcpiWriteGasRaw(UINT64 Addr, UINT8 SpaceId, UINT8 BitWidth, UINT64 Value) {
    if (SpaceId == 0) {
        // System Memory (MMIO)
        UINT64 Virt = PhysToVirt(Addr);
        if (!Virt) return;
        if (BitWidth <= 8) {
            *(volatile UINT8*)Virt = (UINT8)Value;
        } else if (BitWidth <= 16) {
            *(volatile UINT16*)Virt = (UINT16)Value;
        } else if (BitWidth <= 32) {
            *(volatile UINT32*)Virt = (UINT32)Value;
        } else {
            *(volatile UINT64*)Virt = Value;
        }
    } else if (SpaceId == 1) {
        // System I/O
        if (BitWidth <= 8) {
            Outb((UINT16)Addr, (UINT8)Value);
        } else if (BitWidth <= 16) {
            Outw((UINT16)Addr, (UINT16)Value);
        } else if (BitWidth <= 32) {
            Outl((UINT16)Addr, (UINT32)Value);
        }
    }
}

STATIC UINT64 AcpiReadGasRaw(UINT64 Addr, UINT8 SpaceId, UINT8 BitWidth) {
    if (SpaceId == 0) {
        UINT64 Virt = PhysToVirt(Addr);
        if (!Virt) return 0;
        if (BitWidth <= 8)  return *(volatile UINT8*)Virt;
        if (BitWidth <= 16) return *(volatile UINT16*)Virt;
        if (BitWidth <= 32) return *(volatile UINT32*)Virt;
        return *(volatile UINT64*)Virt;
    } else if (SpaceId == 1) {
        if (BitWidth <= 8)  return Inb((UINT16)Addr);
        if (BitWidth <= 16) return Inw((UINT16)Addr);
        if (BitWidth <= 32) return Inl((UINT16)Addr);
    }
    return 0;
}

// Combined recording in the GAS region (auto space + width)
STATIC VOID AcpiWriteGas(UINT8 *Gas, UINT64 Value) {
    if (!Gas || Gas[0] == 0) return;
    UINT8 SpaceId = Gas[0];
    UINT8 BitWidth = Gas[1];
    UINT64 Addr = *(UINT64*)(Gas + 4);

    // GAS can be both Memory and IO — defined by SpaceId
    if (SpaceId == 0 || SpaceId == 1) {
        AcpiWriteGasRaw(Addr, SpaceId, BitWidth, Value);
    }
}

// ============================================================================
// PM1 helpers
// ============================================================================

STATIC UINT16 AcpiPm1ControlValue(UINT8 SlpTyp) {
    // SLP_TYP[2:0] in bits 12:10, SLP_EN — bit 13
    return ((UINT16)(SlpTyp & 0x07) << 10) | (1U << 13);
}

// Disable all wake events and GPE before bedtime
STATIC VOID AcpiDisableWakeEvents(VOID) {
    FADT *Fadt = GAcpi.Fadt;
    if (!Fadt) return;

    // PM1_EN = PM1_STS + 2 (2 bytes)
    if (Fadt->Pm1aEvtBlk) {
        Outw(Fadt->Pm1aEvtBlk + 2, 0x0000);
    }
    if (Fadt->Pm1bEvtBlk) {
        Outw(Fadt->Pm1bEvtBlk + 2, 0x0000);
    }

    // Clearing PM1_STS (write-1-to-clear)
    if (Fadt->Pm1aEvtBlk) Outw(Fadt->Pm1aEvtBlk, 0xFFFF);
    if (Fadt->Pm1bEvtBlk) Outw(Fadt->Pm1bEvtBlk, 0xFFFF);

    // GPE0: GPE0_STS = Gpe0Blk, GPE0_EN = Gpe0Blk + Gpe0BlkLen/2
    if (Fadt->Gpe0Blk && Fadt->Gpe0BlkLen) {
        UINT8 Len = Fadt->Gpe0BlkLen;
        UINT16 Gpe0Sts = Fadt->Gpe0Blk;
        UINT16 Gpe0En  = Fadt->Gpe0Blk + Len / 2;

        for (UINT16 Off = 0; Off + 2 <= Len / 2; Off += 2) {
            Outw(Gpe0Sts + Off, 0xFFFF);
        }
        for (UINT16 Off = 0; Off + 2 <= Len / 2; Off += 2) {
            Outw(Gpe0En + Off, 0xFFFF);
        }
    }
    if (Fadt->Gpe1Blk && Fadt->Gpe1BlkLen) {
        UINT8 Len = Fadt->Gpe1BlkLen;
        UINT16 Gpe1Sts = Fadt->Gpe1Blk;
        UINT16 Gpe1En  = Fadt->Gpe1Blk + Len / 2;

        for (UINT16 Off = 0; Off + 2 <= Len / 2; Off += 2) {
            Outw(Gpe1Sts + Off, 0xFFFF);
        }
        for (UINT16 Off = 0; Off + 2 <= Len / 2; Off += 2) {
            Outw(Gpe1En + Off, 0xFFFF);
        }
    }
}

// Write PM1 Control (SLP_TYP + SLP_EN) to both blocks
STATIC VOID AcpiWritePm1Control(UINT8 SlpTyp) {
    FADT *Fadt = GAcpi.Fadt;
    if (!Fadt) return;

    UINT16 SleepValue = AcpiPm1ControlValue(SlpTyp);

    // PM1a
    UINT64 Pm1aAddr = AcpiReadGas(Fadt->XPm1aCntBlk, NULLPTR);
    UINT8 Pm1aSpace = AcpiGetGasSpaceId(Fadt->XPm1aCntBlk);
    UINT8 Pm1aWidth = AcpiGetGasWidth(Fadt->XPm1aCntBlk);

    if (Pm1aAddr && (Pm1aSpace == 0 || Pm1aSpace == 1)) {
        AcpiWriteGasRaw(Pm1aAddr, Pm1aSpace, Pm1aWidth ? Pm1aWidth : 16, SleepValue);
    } else if (Fadt->Pm1aCntBlk) {
        Outw(Fadt->Pm1aCntBlk, SleepValue);
    }

    // PM1b
    UINT64 Pm1bAddr = AcpiReadGas(Fadt->XPm1bCntBlk, NULLPTR);
    UINT8 Pm1bSpace = AcpiGetGasSpaceId(Fadt->XPm1bCntBlk);
    UINT8 Pm1bWidth = AcpiGetGasWidth(Fadt->XPm1bCntBlk);

    if (Pm1bAddr && (Pm1bSpace == 0 || Pm1bSpace == 1)) {
        AcpiWriteGasRaw(Pm1bAddr, Pm1bSpace, Pm1bWidth ? Pm1bWidth : 16, SleepValue);
    } else if (Fadt->Pm1bCntBlk) {
        Outw(Fadt->Pm1bCntBlk, SleepValue);
    }
}

// ============================================================================
// Public power management
// ============================================================================

VOID AcpiReboot(VOID) {
    //Calling _PTS(5) is not necessary, but some BIOSes expect
    AmlValue Res;
    AmlValue Arg = AmlMakeInteger(5);
    AmlCallMethod("\\_PTS", 1, &Arg, &Res);

    if (GAcpi.Fadt) {
        FADT *Fadt = GAcpi.Fadt;
        UINT8 *ResetReg = Fadt->ResetReg;
        UINT8 ResetValue = Fadt->ResetValue;

        if (ResetReg[0] != 0) {
            UINT8 SpaceId = ResetReg[0];
            UINT8 BitWidth = ResetReg[1];
            UINT64 Addr = *(UINT64*)(ResetReg + 4);

            // Reset Register — usually Generic I/O
            if (SpaceId == 0 || SpaceId == 1) {
                AcpiWriteGasRaw(Addr, SpaceId, BitWidth, ResetValue);
            }
        }
    }

    // Fallback: keyboard controller reset
    Outb(0x64, 0xFE);

    // Give the hardware a chance
    for (INT I = 0; I < 100000; I++) IoWait();

    // If it didn't work, it's a triple mistake.
    for (INT Attempt = 0; Attempt < 3; Attempt++) {
        Outb(0xCF9, 0x06);
        for (INT I = 0; I < 10000; I++) IoWait();
    }

    while (1) Halt();
}

VOID AcpiShutdown(VOID) {
    if (!GAcpi.Fadt) {
        // No FADT — we can't switch off
        while (1) Halt();
    }

    FADT *Fadt = GAcpi.Fadt;

    // 1. Calling _PTS(5) — Prepare To Sleep S5
    // _PTS is called AFTER interrupts are disabled, but BEFORE writing to PM1.
    // Not all BIOSes have _PTS, so we ignore the result.
    LocalInterruptsDisable();

    AmlValue Arg = AmlMakeInteger(5);
    AmlValue Res;
    AmlCallMethod("\\_PTS", 1, &Arg, &Res);

    // Disabling wake events and GPE
    AcpiDisableWakeEvents();

    // Let the gland calm down
    for (INT I = 0; I < 10000; I++) IoWait();

    // 4. Get SLP_TYP for S5 from AML
    UINT8 SlpTyp = 5;   // default fallback
    BOOL HaveAml = AmlGetS5SlpTyp(&SlpTyp);

    // Let's put together a list of candidates: first AML, then fallback
    UINT8 Candidates[8];
    UINT8 NumCandidates = 0;

    if (HaveAml) {
        Candidates[NumCandidates++] = SlpTyp;
    }

    // Standard SLP_TYP values for S5 (all possible):
    // 0, 1, 4, 5, 6, 7 — depends on the chipset
    STATIC const UINT8 Fallbacks[] = { 5, 0, 7, 1, 4, 6 };
    for (UINT8 I = 0; I < sizeof(Fallbacks); I++) {
        if (HaveAml && Fallbacks[I] == SlpTyp) continue;
        if (NumCandidates >= sizeof(Candidates)) break;
        Candidates[NumCandidates++] = Fallbacks[I];
    }

    // 5. We try every SLP_TYP
    for (UINT8 Idx = 0; Idx < NumCandidates; Idx++) {
        AcpiWritePm1Control(Candidates[Idx]);

        // If SLP_EN worked, we won't be coming back here.
        // If SLP_EN works, we won't be coming back here.
        for (INT I = 0; I < 200000; I++) IoWait();
    }

    // Nothing worked.
    while (1) Halt();
}

// ============================================================================
// Sleep (S1..S4)
// ============================================================================

// An auxiliary function is to prepare the system for sleep.
STATIC VOID AcpiPrepareForSleep(UINT8 SleepState) {
    LocalInterruptsDisable();

    // Calling _PTS(SleepState) — Prepare To Sleep
    AmlValue Arg = AmlMakeInteger(SleepState);
    AmlValue Res;
    AmlCallMethod("\\_PTS", 1, &Arg, &Res);

    // Disable wake events and GPE
    AcpiDisableWakeEvents();

    for (INT I = 0; I < 10000; I++) IoWait();
}

// Falling asleep S1..S4
// SleepState: 1, 2, 3, 4
BOOL AcpiSleep(UINT8 SleepState) {
    if (!GAcpi.Fadt) return FALSE;
    if (SleepState < 1 || SleepState > 5) return FALSE;

    // S5 = shutdown, processed separately
    if (SleepState == 5) {
        AcpiShutdown();
        return TRUE;
    }

    // Get SLP_TYP from AML (\_S1,\_S2, \_S3, \_S4)
    CHAR Name[5];
    SnPrintf(Name, sizeof(Name), "_S%u", SleepState);

    UINT8 SlpTyp = 0;
    BOOL HaveAml = AmlGetSleepSlpTyp(Name, &SlpTyp);
    if (!HaveAml) {
        // There is no method — the state is not maintained.
        return FALSE;
    }

    AcpiPrepareForSleep(SleepState);

    // Record PM1 Control
    AcpiWritePm1Control(SlpTyp);

    // If we returned, the dream failed.
    for (INT I = 0; I < 200000; I++) IoWait();

    LocalInterruptsEnable();
    return FALSE;
}

SysDevice *AcpiGetSysDevice(VOID) {
    return GAcpiDevice;
}

STATIC SysDevice *AcpiCreateChild(const CHAR *Name,
                                   const CHAR *Desc,
                                   SysDeviceType Type) {
    SysDevice *Parent = GAcpiDevice;
    if (!Parent) Parent = SysDeviceGetRoot();
    if (!Parent) return NULLPTR;
    return SysDeviceCreate(Parent, Name, Desc, Type);
}

STATIC VOID AcpiRegBus(VOID) {
    SysDevice *Root = SysDeviceGetRoot();
    if (!Root) return;

    GAcpiDevice = SysDeviceCreate(Root, "ACPI", "ACPI bus", SYSDEV_TYPE_BUS);
    if (!GAcpiDevice) return;

    SysDeviceSetProp(GAcpiDevice, "revision",
                     GAcpi.UseXsdt ? "2.0+" : "1.0");
    SysDeviceSetPropHex(GAcpiDevice, "rsdp", (UINT64)(UINTPTR)GAcpi.Rsdp);

    if (GAcpi.Xsdt) {
        SysDeviceSetPropHex(GAcpiDevice, "xsdt", (UINT64)(UINTPTR)GAcpi.Xsdt);
    } else if (GAcpi.Rsdt) {
        SysDeviceSetPropHex(GAcpiDevice, "rsdt", (UINT64)(UINTPTR)GAcpi.Rsdt);
    }

    SysDeviceSetPropU32(GAcpiDevice, "ssdt_count", (UINT32)GAcpi.SsdtCount);
    SysDeviceSetState(GAcpiDevice, SYSDEV_STATE_ENABLED);

    KDriver *Drv = KDriverFindByName("ACPI");
    if (Drv) SysDeviceBindDriver(GAcpiDevice, Drv);
}

STATIC VOID AcpiRegFadt(VOID) {
    if (!GAcpi.Fadt) return;
    FADT *F = GAcpi.Fadt;

    SysDevice *Pm = AcpiCreateChild("PM", "ACPI Power Management",
                                     SYSDEV_TYPE_POWER);
    if (!Pm) return;

    if (F->Pm1aEvtBlk) {
        SysDeviceAddResource(Pm, SYSDEV_RES_IO, F->Pm1aEvtBlk, F->Pm1EvtLen);
        SysDeviceSetPropHex(Pm, "pm1a_evt_blk", F->Pm1aEvtBlk);
    }
    if (F->Pm1bEvtBlk) {
        SysDeviceAddResource(Pm, SYSDEV_RES_IO, F->Pm1bEvtBlk, F->Pm1EvtLen);
        SysDeviceSetPropHex(Pm, "pm1b_evt_blk", F->Pm1bEvtBlk);
    }
    if (F->Pm1aCntBlk) {
        SysDeviceAddResource(Pm, SYSDEV_RES_IO, F->Pm1aCntBlk, F->Pm1CntLen);
        SysDeviceSetPropHex(Pm, "pm1a_cnt_blk", F->Pm1aCntBlk);
    }
    if (F->Pm1bCntBlk) {
        SysDeviceAddResource(Pm, SYSDEV_RES_IO, F->Pm1bCntBlk, F->Pm1CntLen);
        SysDeviceSetPropHex(Pm, "pm1b_cnt_blk", F->Pm1bCntBlk);
    }
    if (F->PmTmrBlk) {
        SysDeviceAddResource(Pm, SYSDEV_RES_IO, F->PmTmrBlk, F->PmTmrLen);
        SysDeviceSetPropHex(Pm, "pm_tmr_blk", F->PmTmrBlk);
    }
    if (F->Gpe0Blk) {
        SysDeviceAddResource(Pm, SYSDEV_RES_IO, F->Gpe0Blk, F->Gpe0BlkLen);
        SysDeviceSetPropHex(Pm, "gpe0_blk", F->Gpe0Blk);
    }
    if (F->Gpe1Blk) {
        SysDeviceAddResource(Pm, SYSDEV_RES_IO, F->Gpe1Blk, F->Gpe1BlkLen);
        SysDeviceSetPropHex(Pm, "gpe1_blk", F->Gpe1Blk);
    }

    if (F->SciInt) {
        SysDeviceAddResource(Pm, SYSDEV_RES_IRQ, F->SciInt, 1);
        SysDeviceSetPropU32(Pm, "sci_irq", F->SciInt);
    }

    SysDeviceSetPropU32(Pm, "pm_profile", F->PreferredPmProfile);
    SysDeviceSetPropU32(Pm, "flags", F->Flags);

    if (F->ResetReg[0] != 0) {
        SysDeviceSetPropHex(Pm, "reset_addr", *(UINT64*)(F->ResetReg + 4));
        SysDeviceSetPropU32(Pm, "reset_space", F->ResetReg[0]);
        SysDeviceSetPropU32(Pm, "reset_value", F->ResetValue);
    }

    if (GAcpi.Dsdt) {
        SysDeviceSetPropHex(Pm, "dsdt", (UINT64)(UINTPTR)GAcpi.Dsdt);
    }

    SysDeviceSetState(Pm, SYSDEV_STATE_ENABLED);
}

STATIC VOID AcpiRegMadt(VOID) {
    if (!GAcpi.Madt) return;

    SysDevice *Madt = AcpiCreateChild("MADT",
                                       "Multiple APIC Description Table",
                                       SYSDEV_TYPE_BUS);
    if (!Madt) return;

    SysDeviceSetPropHex(Madt, "local_apic_addr",
                        GAcpi.Madt->LocalApicAddress);
    SysDeviceSetPropU32(Madt, "flags", GAcpi.Madt->Flags);
    SysDeviceSetPropU32(Madt, "cpu_count", GAcpi.Apic.ProcessorCount);
    SysDeviceSetPropU32(Madt, "ioapic_count", GAcpi.Apic.IoApicCount);
    SysDeviceSetPropU32(Madt, "override_count", GAcpi.Apic.IntOverrideCount);

    SysDeviceSetState(Madt, SYSDEV_STATE_ENABLED);
}

STATIC VOID AcpiRegMcfg(VOID) {
    if (!GAcpi.Mcfg) return;

    SysDevice *Dev = AcpiCreateChild("MCFG",
                                      "PCIe Memory Mapped Config",
                                      SYSDEV_TYPE_BUS);
    if (!Dev) return;

    SysDeviceSetPropHex(Dev, "mcfg_addr", (UINT64)(UINTPTR)GAcpi.Mcfg);
    SysDeviceSetPropU32(Dev, "length", GAcpi.Mcfg->Header.Length);
    SysDeviceSetState(Dev, SYSDEV_STATE_ENABLED);
}

SysDevice *AcpiRegisterSysDevices(VOID) {
    if (GAcpiDevice) return GAcpiDevice;   // already registered
    if (!SysDeviceGetRoot()) return NULLPTR; // SysDevice is not ready yet

    AcpiRegBus();
    AcpiRegFadt();
    AcpiRegMadt();
    AcpiRegMcfg();

    return GAcpiDevice;
}