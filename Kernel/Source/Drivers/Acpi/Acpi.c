#include <Drivers/Acpi/Acpi.h>
#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Mem/Paging.h>
#include <SysDevice.h>
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
    
    KDriverRegister(KDriverGenerateStruct("ACPI", 0, TRUE, &GAcpi, NULLPTR));

    AcpiRegisterSysDevices();
    
    RETURN(SUCCESS);
}

// Вспомогательная функция для Generic Address Structure
STATIC UINT64 AcpiReadGas(UINT8 *Gas, UINT8 *WidthOut) {
    if (!Gas || Gas[0] == 0) return 0;
    
    UINT8 SpaceId = Gas[0];
    UINT8 BitWidth = Gas[1];
    UINT8 BitOffset = Gas[2];
    UINT64 Addr = *(UINT64*)(Gas + 4);
    
    if (WidthOut) *WidthOut = BitWidth;
    
    // Только System I/O и System Memory
    if (SpaceId != 1 && SpaceId != 0) return 0;
    
    return Addr;
}

// Запись в ACPI регистр через GAS
STATIC VOID AcpiWriteGas(UINT8 *Gas, UINT32 Value) {
    if (!Gas || Gas[0] == 0) return;
    
    UINT8 SpaceId = Gas[0];
    UINT8 BitWidth = Gas[1];
    UINT64 Addr = *(UINT64*)(Gas + 4);
    
    if (SpaceId == 1) { // System I/O
        if (BitWidth <= 8) {
            Outb((UINT16)Addr, (UINT8)Value);
        } else if (BitWidth <= 16) {
            Outw((UINT16)Addr, (UINT16)Value);
        } else if (BitWidth <= 32) {
            Outl((UINT16)Addr, Value);
        }
    } else if (SpaceId == 0) { // System Memory
        UINT64 Virt = PhysToVirt(Addr);
        if (BitWidth <= 8) {
            *(volatile UINT8*)Virt = (UINT8)Value;
        } else if (BitWidth <= 16) {
            *(volatile UINT16*)Virt = (UINT16)Value;
        } else if (BitWidth <= 32) {
            *(volatile UINT32*)Virt = Value;
        }
    }
}

VOID AcpiReboot(VOID) {
    if (!GAcpi.Fadt) {
        Outb(0x64, 0xFE);
        return;
    }
    
    // Используем Reset Register через GAS
    UINT8 *ResetReg = GAcpi.Fadt->ResetReg;
    UINT8 ResetValue = GAcpi.Fadt->ResetValue;
    
    if (ResetReg[0] != 0) {
        AcpiWriteGas(ResetReg, ResetValue);
    } else {
        // Fallback: keyboard controller reset
        Outb(0x64, 0xFE);
    }
    
    // Задержка
    for (INT I = 0; I < 100000; I++) {
        IoWait();
    }
    
    while(1) {
        Halt();
    }
}

VOID AcpiShutdown(VOID) {
    if (!GAcpi.Fadt) {
        return;
    }
    
    FADT *Fadt = GAcpi.Fadt;
    
    // 1. Получаем SLP_TYP для S5 из FADT
    // В ACPI 1.0 - фиксированное значение 5
    // В ACPI 2.0+ может быть в X_PM1aCntBlk
    UINT8 SleepTypeS5 = 5; // По умолчанию
    
    // 2. Находим PM1a Control Register (через GAS если доступен)
    UINT8 *Pm1aCnt = Fadt->XPm1aCntBlk;
    UINT16 Pm1aPort = Fadt->Pm1aCntBlk;
    
    // Если есть X_PM1aCntBlk - используем его
    UINT64 Pm1aAddr = AcpiReadGas(Pm1aCnt, NULLPTR);
    BOOL UseGas = (Pm1aAddr != 0);
    
    // 3. Находим PM1b Control Register
    UINT8 *Pm1bCnt = Fadt->XPm1bCntBlk;
    UINT16 Pm1bPort = Fadt->Pm1bCntBlk;
    UINT64 Pm1bAddr = AcpiReadGas(Pm1bCnt, NULLPTR);
    
    // 4. Отключаем все пробуждения (GPE)
    if (Fadt->Gpe0Blk) {
        Outw(Fadt->Gpe0Blk, 0); // GPE0_EN
    }
    if (Fadt->Gpe1Blk) {
        Outw(Fadt->Gpe1Blk, 0); // GPE1_EN
    }
    
    // 5. Маскируем все прерывания
    LocalInterruptsDisable();
    
    // 6. Небольшая задержка
    for (INT I = 0; I < 1000; I++) {
        IoWait();
    }
    
    // 7. Записываем SLP_TYP_S5 | SLP_EN
    UINT16 SleepValue = (UINT16)((SleepTypeS5 << 10) | (1 << 13)); // SLP_TYP_S5 + SLP_EN
    
    // Пишем в PM1a
    if (UseGas) {
        AcpiWriteGas(Pm1aCnt, SleepValue);
    } else if (Pm1aPort) {
        Outw(Pm1aPort, SleepValue);
    }
    
    // Пишем в PM1b (если есть)
    if (Pm1bAddr) {
        AcpiWriteGas(Pm1bCnt, SleepValue);
    } else if (Pm1bPort) {
        Outw(Pm1bPort, SleepValue);
    }
    
    // 8. КРИТИЧЕСКИ ВАЖНО: ждём завершения перехода!
    // Даём контроллеру время на переход в S5
    for (INT I = 0; I < 500000; I++) {
        IoWait();
    }
    
    // 9. Финальная задержка
    for (INT I = 0; I < 100000; I++) {
        IoWait();
    }
    
    // 10. Если ещё живы - вешаемся
    while(1) {
        Halt();
    }
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
    if (GAcpiDevice) return GAcpiDevice;   // уже зарегистрировано
    if (!SysDeviceGetRoot()) return NULLPTR; // SysDevice ещё не готов

    AcpiRegBus();
    AcpiRegFadt();
    AcpiRegMadt();
    AcpiRegMcfg();

    return GAcpiDevice;
}