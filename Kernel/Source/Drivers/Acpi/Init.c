#include <Drivers/Irq/Apic/Ioapic.h>
#include <Drivers/Irq/Apic/Apic.h>
#include <Limine/LimineParse.h>
#include <Drivers/Acpi/Acpi.h>
#include <Drivers/Acpi/Sci.h>
#include <Mem/Paging.h>
#include <Lib/String.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <Return.h>
#include <Asm/Io.h>
#include <Types.h>

STATIC UINT64 FindRsdp(VOID) {
    UINT64 RsdpAddr = LimineGetRsdp();

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
    UINT16 *EbdaSeg = (UINT16*)PhysToVirt(0x40E);
    UINT32 EbdaAddr = (*EbdaSeg) << 4;
    
    if (EbdaAddr >= 0x80000 && EbdaAddr < 0xA0000) {
        Ptr = (UINT8*)PhysToVirt(EbdaAddr);
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
            UINT8 *TestPtr = (UINT8*)PhysToVirt(Addr);
            if (MemCmp(TestPtr, "RSD PTR ", 8) == 0) {
                RSDPV2 *Rsdp = (RSDPV2*)Addr;
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
        
        UINT8 *TestPtr = (UINT8*)PhysToVirt(Addr);
        if (MemCmp(TestPtr, "RSD PTR ", 8) == 0) {
            RSDPV2 *Rsdp = (RSDPV2*)Addr;
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
    
    StdOutPrintf("acpi: searching for rsdp... ");
    
    UINT64 RsdpAddr = FindRsdp();
    if (!RsdpAddr) {
        StdOutPrintf("not found\n");
        StdErrPrintf("acpi: rsdp not found\n");
        RETURN(NO_OBJECT);
    }
    
    StdOutPrintf("found at 0x%X\n", (UINT32)RsdpAddr);
    
    
    StdOutPrintf("acpi: initializing... ");
    
    Result = AcpiInit(RsdpAddr);
    if (Result != SUCCESS) {
        StdOutPrintf("failed (error %d)\n", Result);
        RETURN(Result);
    }
    
    StdOutPrintf("done\n");
    
    // Table Information
    Acpi *AcpiObj = AcpiGet();
    if (AcpiObj) {
        StdOutPrintf("acpi: using %s\n", 
                      AcpiObj->UseXsdt ? "xsdt" : "rsdt");
        if (AcpiObj->Fadt) {
            StdOutPrintf("acpi: fadt revision %d\n", 
                          AcpiObj->Fadt->Header.Revision);
        }
        if (AcpiObj->Madt) {
            StdOutPrintf("acpi: madt found\n");
        }
        if (AcpiObj->Dsdt) {
            StdOutPrintf("acpi: dsdt found\n");
        }
        StdOutPrintf("acpi: %d ssdt's\n", AcpiObj->SsdtCount);
    }
    
    
    UINT32 ProcCount = AcpiGetProcessorCount();
    UINT32 IoApicCount = AcpiGetIoApicCount();
    UINT32 OverrideCount = AcpiGetOverrideCount();
    
    StdOutPrintf("acpi: %d processors found\n", ProcCount);
    StdOutPrintf("acpi: %d ioapic's found\n", IoApicCount);
    StdOutPrintf("acpi: %d interrupt overrides\n", OverrideCount);
    
    // Processor information
    for (UINT32 I = 0; I < ProcCount; I++) {
        const AcpiProcessor *Proc = AcpiGetProcessor(I);
        if (Proc) {
            StdOutPrintf("acpi: cpu %d: apic id %d, %s, %s\n",
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
            StdOutPrintf("acpi: ioapic %d: id %d, addr 0x%X, gsi base %d\n",
                          I,
                          Ioapic->Id,
                          Ioapic->Address,
                          Ioapic->GsiBase);
        }
    }
    
    StdOutPrintf("apic: initializing... ");
    
    Result = ApicInit();
    if (Result != SUCCESS) {
        StdOutPrintf("failed (error %d)\n", Result);
        RETURN(Result);
    }
    
    StdOutPrintf("done\n");
    StdOutPrintf("apic: bsp id %d\n", ApicGetId());
    StdOutPrintf("apic: version %d\n", ApicGetVersion());
    StdOutPrintf("apic: mode %s\n", 
                  ApicIsX2ApicMode() ? "x2apic" : "xapic");
    StdOutPrintf("apic: x2apic support %s\n",
                  ApicCpuSupportsX2Apic() ? "yes" : "no");
    
    // Enable APIC
    StdOutPrintf("apic: enabling... ");
    ApicEnable();
    if (ApicIsEnabled()) {
        StdOutPrintf("done\n");
    } else {
        StdOutPrintf("failed\n");
    }
    
    StdOutPrintf("ioapic: initializing... ");
    
    Result = IoapicInit();
    if (Result != SUCCESS) {
        StdOutPrintf("failed (error %d)\n", Result);
        StdOutPrintf("ioapic: initialization failed, interrupts may not work\n");
        RETURN(Result);
    }
    
    StdOutPrintf("done\n");
    StdOutPrintf("ioapic: %d devices\n", IoapicGetCount());
    
    // Information about IOAPIC devices
    UINT32 IoapicDevCount = IoapicGetCount();
    for (UINT32 I = 0; I < IoapicDevCount; I++) {
        IoapicDevice *Dev = IoapicGetDevice(I);
        if (Dev) {
            StdOutPrintf("ioapic: device %d: id %d, gsi base %d, %d irq's\n",
                          I,
                          Dev->Id,
                          Dev->GsiBase,
                          Dev->MaxRedir);
        }
    }
    
    // Masking all IRQs
    IoapicMaskAll();

    AcpiSciInit();
    
    RETURN(SUCCESS);
}
