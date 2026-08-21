#include <Types.h>
#include <Basecon.h>
#include <Drivers/Acpi/Acpi.h>
#include <Drivers/Apic/Apic.h>
#include <Drivers/Apic/Ioapic.h>
#include <Limine/LimineParse.h>
#include <Lib/String.h>
#include <Return.h>
#include <Asm/Io.h>

static UINT64 FindRsdp(VOID) {
    UINT64 RsdpAddr = 0;
    
    RsdpAddr = LimineGetRsdp();
    if (RsdpAddr) {
        return RsdpAddr;
    }
    
    BOOL IsUefiBoot = IsUefi();
    
    UINT8 *Ptr = (UINT8*)0xE0000;
    UINT8 *End = (UINT8*)0x100000;
    
    while (Ptr < End) {
        if (MemCmp(Ptr, "RSD PTR ", 8) == 0) {
            // Validating the checksum
            RSDPV2 *Rsdp = (RSDPV2*)(UINTPTR)Ptr;
            if (AcpiChecksum(Rsdp, ACPI_RSDP_V1_SIZE) == 0) {
                RsdpAddr = (UINT64)(UINTPTR)Ptr;
                return RsdpAddr;
            }
        }
        Ptr += 16;
    }
    
    // We get the EBDA address (0x40E in BDA)
    UINT16 *EbdaSeg = (UINT16*)0x40E;
    UINT32 EbdaAddr = (*EbdaSeg) << 4;
    
    if (EbdaAddr >= 0x80000 && EbdaAddr < 0xA0000) {
        Ptr = (UINT8*)(UINTPTR)EbdaAddr;
        End = Ptr + 1024;  // EBDA maximum 1KB
        
        while (Ptr < End) {
            if (MemCmp(Ptr, "RSD PTR ", 8) == 0) {
                RSDPV2 *Rsdp = (RSDPV2*)(UINTPTR)Ptr;
                if (AcpiChecksum(Rsdp, ACPI_RSDP_V1_SIZE) == 0) {
                    RsdpAddr = (UINT64)(UINTPTR)Ptr;
                    return RsdpAddr;
                }
            }
            Ptr += 16;
        }
    }
    
    if (IsUefiBoot) {
        // In UEFI RSDP is usually in the area 0x80000000 - 0xFFFFFFFF
        // Searching with 16-byte alignment
        for (UINT64 Addr = 0x80000000; Addr < 0xFFFFFFFF; Addr += 16) {
            if (Addr + 8 > 0xFFFFFFFF) break;
            
            // Checking the signature
            UINT8 *TestPtr = (UINT8*)(UINTPTR)Addr;
            if (MemCmp(TestPtr, "RSD PTR ", 8) == 0) {
                RSDPV2 *Rsdp = (RSDPV2*)(UINTPTR)Addr;
                if (AcpiChecksum(Rsdp, ACPI_RSDP_V1_SIZE) == 0) {
                    RsdpAddr = Addr;
                    return RsdpAddr;
                }
            }
        }
    }
    
    // Search only in aligned addresses (16-byte alignment)
    for (UINT64 Addr = 0x00000000; Addr < 0xFFFFFFFF; Addr += 16) {
        // Skip already checked areas
        if (Addr >= 0x000E0000 && Addr < 0x00100000) continue;
        if (Addr >= 0x00080000 && Addr < 0x000A0000) continue;
        
        // We skip the PCI/MMIO area (usually there is no RSDP there)
        if (Addr >= 0xA0000000 && Addr < 0xE0000000) continue;
        
        UINT8 *TestPtr = (UINT8*)(UINTPTR)Addr;
        if (MemCmp(TestPtr, "RSD PTR ", 8) == 0) {
            RSDPV2 *Rsdp = (RSDPV2*)(UINTPTR)Addr;
            if (AcpiChecksum(Rsdp, ACPI_RSDP_V1_SIZE) == 0) {
                RsdpAddr = Addr;
                return RsdpAddr;
            }
        }
    }
    return 0;
}

INT InitAcpiAndApic(VOID) {
    Outb(0x21, 0xFF);
    Outb(0xA1, 0xFF);

    INT Result;
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "acpi: searching for rsdp... ");
    
    UINT64 RsdpAddr = FindRsdp();
    if (!RsdpAddr) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "not found\n");
        BaseconPrintf(BASECON_TYPE_ERROR, "acpi: rsdp not found\n");
        RETURN(NO_OBJECT);
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "found at 0x%X\n", (UINT32)RsdpAddr);
    
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "acpi: initializing... ");
    
    Result = AcpiInit(RsdpAddr);
    if (Result != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "failed (error %d)\n", Result);
        RETURN(Result);
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "done\n");
    
    // Table Information
    Acpi *AcpiObj = AcpiGet();
    if (AcpiObj) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "acpi: using %s\n", 
                      AcpiObj->UseXsdt ? "xsdt" : "rsdt");
        if (AcpiObj->Fadt) {
            BaseconPrintf(BASECON_TYPE_NORMAL, "acpi: fadt revision %d\n", 
                          AcpiObj->Fadt->Header.Revision);
        }
        if (AcpiObj->Madt) {
            BaseconPrintf(BASECON_TYPE_NORMAL, "acpi: madt found\n");
        }
        if (AcpiObj->Dsdt) {
            BaseconPrintf(BASECON_TYPE_NORMAL, "acpi: dsdt found\n");
        }
        BaseconPrintf(BASECON_TYPE_NORMAL, "acpi: %d ssdt's\n", AcpiObj->SsdtCount);
    }
    
    
    UINT32 ProcCount = AcpiGetProcessorCount();
    UINT32 IoApicCount = AcpiGetIoApicCount();
    UINT32 OverrideCount = AcpiGetOverrideCount();
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "acpi: %d processors found\n", ProcCount);
    BaseconPrintf(BASECON_TYPE_NORMAL, "acpi: %d ioapic's found\n", IoApicCount);
    BaseconPrintf(BASECON_TYPE_NORMAL, "acpi: %d interrupt overrides\n", OverrideCount);
    
    // Processor information
    for (UINT32 I = 0; I < ProcCount; I++) {
        const AcpiProcessor *Proc = AcpiGetProcessor(I);
        if (Proc) {
            BaseconPrintf(BASECON_TYPE_NORMAL, 
                          "acpi: cpu %d: apic id %d, %s, %s\n",
                          I,
                          Proc->ApicId,
                          Proc->Enabled ? "enabled" : "disabled",
                          Proc->X2Apic ? "x2apic" : "xapic");
        }
    }
    
    // Information about IOAPIC
    for (UINT32 I = 0; I < IoApicCount; I++) {
        const AcpiIoApic *Ioapic = AcpiGetIoApic(I);
        if (Ioapic) {
            BaseconPrintf(BASECON_TYPE_NORMAL,
                          "acpi: ioapic %d: id %d, addr 0x%X, gsi base %d\n",
                          I,
                          Ioapic->Id,
                          Ioapic->Address,
                          Ioapic->GsiBase);
        }
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "apic: initializing... ");
    
    Result = ApicInit();
    if (Result != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "failed (error %d)\n", Result);
        RETURN(Result);
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "done\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "apic: bsp id %d\n", ApicGetId());
    BaseconPrintf(BASECON_TYPE_NORMAL, "apic: version %d\n", ApicGetVersion());
    BaseconPrintf(BASECON_TYPE_NORMAL, "apic: mode %s\n", 
                  ApicIsX2ApicMode() ? "x2apic" : "xapic");
    BaseconPrintf(BASECON_TYPE_NORMAL, "apic: x2apic support %s\n",
                  ApicCpuSupportsX2Apic() ? "yes" : "no");
    
    // Enable APIC
    BaseconPrintf(BASECON_TYPE_NORMAL, "apic: enabling... ");
    ApicEnable();
    if (ApicIsEnabled()) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "done\n");
    } else {
        BaseconPrintf(BASECON_TYPE_NORMAL, "failed\n");
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "ioapic: initializing... ");
    
    Result = IoapicInit();
    if (Result != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "failed (error %d)\n", Result);
        BaseconPrintf(BASECON_TYPE_NORMAL, "ioapic: initialization failed, interrupts may not work\n");
        RETURN(Result);
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "done\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "ioapic: %d devices\n", IoapicGetCount());
    
    // Information about IOAPIC devices
    UINT32 IoapicDevCount = IoapicGetCount();
    for (UINT32 I = 0; I < IoapicDevCount; I++) {
        IoapicDevice *Dev = IoapicGetDevice(I);
        if (Dev) {
            BaseconPrintf(BASECON_TYPE_NORMAL,
                          "ioapic: device %d: id %d, gsi base %d, %d irq's\n",
                          I,
                          Dev->Id,
                          Dev->GsiBase,
                          Dev->MaxRedir);
        }
    }
    
    // Masking all IRQs
    IoapicMaskAll();
    
    RETURN(SUCCESS);
}
