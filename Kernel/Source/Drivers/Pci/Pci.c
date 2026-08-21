#include <Drivers/Pci.h>
#include <Asm/Io.h>
#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Types.h>
#include <KDriver.h>
#include <Return.h>

VOID PciScanBus(UINT8 Bus);
UINT32 PciReadAddr(UINT8 Bus, UINT8 Slot, UINT8 Func, UINT8 Offset);

// ==================== GLOBAL VARIABLES ====================
PciDevice* PciDevices = NULLPTR;
static INT DeviceCount = 0;

// ==================== BASIC OPERATIONS ====================

UINT32 PciReadAddr(UINT8 Bus, UINT8 Slot, UINT8 Func, UINT8 Offset) {
    // Forming an address
    UINT32 Address = (UINT32)((Bus << 16) | (Slot << 11) | (Func << 8) | (Offset & 0xFC) | 0x80000000);
    
    // Send address
    Outl(0xCF8, Address);
    
    // Reading the data
    return Inl(0xCFC);
}

// Formation of address
static UINT32 PciMakeAddr(UINT8 Bus, UINT8 Slot, UINT8 Func, UINT8 Offset) {
    return (1u << 31) | (Bus << 16) | (Slot << 11) | (Func << 8) | (Offset & 0xFC);
}

// Reading configuration
UINT32 PciRead(PciDevice *Dev, UINT8 Offset) {
    UINT32 Addr = PciMakeAddr(Dev->Bus, Dev->Slot, Dev->Function, Offset);
    Outl(PCI_CONFIG_ADDRESS, Addr);
    return Inl(PCI_CONFIG_DATA);
}

// Write configuration
VOID PciWrite(PciDevice *Dev, UINT8 Offset, UINT32 Value) {
    UINT32 Addr = PciMakeAddr(Dev->Bus, Dev->Slot, Dev->Function, Offset);
    Outl(PCI_CONFIG_ADDRESS, Addr);
    Outl(PCI_CONFIG_DATA, Value);
}

// Fast field reading
UINT16 PciReadVendor(UINT8 Bus, UINT8 Slot, UINT8 Func) {
    UINT32 Addr = PciMakeAddr(Bus, Slot, Func, 0x00);
    Outl(PCI_CONFIG_ADDRESS, Addr);
    return Inl(PCI_CONFIG_DATA) & 0xFFFF;
}

// ==================== PCI/PCIe DETECTION ====================

// Search Capability
UINT8 PciFindCap(PciDevice *Dev, UINT8 CapId) {
    if (!Dev) return 0;
    
    // Read the status to make sure the Capabilities List is supported
    UINT16 Status = PciRead(Dev, 0x04) >> 16;
    if (!(Status & (1 << 4))) return 0;  // Capabilities List is not supported
    
    UINT8 CapPtr = (PciRead(Dev, 0x34) >> 8) & 0xFC;  // Low-order 2 bits are reserved
    if (!CapPtr) return 0;
    
    UINT8 Offset = CapPtr;
    while (Offset >= 0x40 && Offset < 0xFF) {
        UINT8 Id = PciRead(Dev, Offset) & 0xFF;
        if (Id == CapId) return Offset;
        Offset = (PciRead(Dev, Offset + 1) >> 8) & 0xFC;
    }
    return 0;
}

// PCIe detection
static VOID PciDetectPciE(PciDevice *Dev) {
    Dev->PciECapOffset = PciFindCap(Dev, 0x10); //  PCIe Capability ID
    
    if (!Dev->PciECapOffset) {
        Dev->IsPciE = FALSE;
        return;
    }
    
    Dev->IsPciE = TRUE;
    
    // Reading PCIe Capability
    UINT32 PciECap = PciRead(Dev, Dev->PciECapOffset);
    Dev->PciEVersion = (PciECap >> 16) & 0xF;
    Dev->PciEType = (PciECap >> 20) & 0xF;
    
    //  Link Capabilities
    UINT32 LinkCap = PciRead(Dev, Dev->PciECapOffset + 0x0C);
    Dev->MaxLinkSpeed = LinkCap & 0xF;
    Dev->MaxLinkWidth = (LinkCap >> 4) & 0x3F;
    
    //  Link Status
    UINT16 LinkStatus = PciRead(Dev, Dev->PciECapOffset + 0x12) & 0xFFFF;
    Dev->LinkSpeed = LinkStatus & 0xF;
    Dev->LinkWidth = (LinkStatus >> 4) & 0x3F;
    
    //  MSI/MSI-X
    Dev->MsiCapOffset = PciFindCap(Dev, 0x05); //  MSI
    Dev->MsiXCapOffset = PciFindCap(Dev, 0x11); //  MSI-X
}

// Detecting BAR registers
static VOID PciDetectBars(PciDevice *Dev) {
    // Save the team
    UINT16 SavedCmd = Dev->Command;
    PciWrite(Dev, 0x04, SavedCmd & ~0x07); // Disable I/O and Memory
    
    for (INT I = 0; I < 6; I++) {
        UINT8 Offset = 0x10 + I * 4;
        UINT32 Bar = PciRead(Dev, Offset);
        
        if (Bar == 0) {
            Dev->Bars[I] = 0;
            Dev->BarSizes[I] = 0;
            Dev->BarTypes[I] = 0;
            continue;
        }
        
        // Determining the BAR type
        if (Bar & 0x01) {
            //  I/O Space BAR
            Dev->BarTypes[I] = 2;
            PciWrite(Dev, Offset, 0xFFFFFFFF);
            UINT32 Size = PciRead(Dev, Offset);
            Size = ~(Size & 0xFFFFFFFC) + 1;
            PciWrite(Dev, Offset, Bar);
            Dev->Bars[I] = Bar & 0xFFFFFFFC;
            Dev->BarSizes[I] = Size;
        } else {
            //  Memory Space BAR
            UINT8 Type = (Bar >> 1) & 0x03;
            
            if (Type == 0x00) { //  32-bit
                Dev->BarTypes[I] = 0;
                PciWrite(Dev, Offset, 0xFFFFFFFF);
                UINT32 Size = PciRead(Dev, Offset);
                Size = ~(Size & 0xFFFFFFF0) + 1;
                PciWrite(Dev, Offset, Bar);
                Dev->Bars[I] = Bar & 0xFFFFFFF0;
                Dev->BarSizes[I] = Size;
            } else if (Type == 0x02) { //  64-bit
                Dev->BarTypes[I] = 1;
                if (I < 5) {
                    // Processing as 64-bit
                    UINT32 BarHigh = PciRead(Dev, Offset + 4);
                    UINT64 FullBar = ((UINT64)BarHigh << 32) | (Bar & 0xFFFFFFF0);
                    
                    // We write down all units
                    PciWrite(Dev, Offset, 0xFFFFFFFF);
                    PciWrite(Dev, Offset + 4, 0xFFFFFFFF);
                    
                    UINT32 SizeLow = PciRead(Dev, Offset);
                    UINT32 SizeHigh = PciRead(Dev, Offset + 4);
                    UINT64 FullSize = ((UINT64)SizeHigh << 32) | (SizeLow & 0xFFFFFFF0);
                    FullSize = ~FullSize + 1;
                    
                    // We restore
                    PciWrite(Dev, Offset, Bar);
                    PciWrite(Dev, Offset + 4, BarHigh);
                    
                    Dev->Bars[I] = FullBar;
                    Dev->BarSizes[I] = FullSize;
                    I++; // Skip the next BAR
                }
            }
        }
    }
    
    // Rebuilding the team
    PciWrite(Dev, 0x04, SavedCmd);
}

// Single device discovery
static PciDevice* PciProbe(UINT8 Bus, UINT8 Slot, UINT8 Func) {
    UINT16 Vendor = PciReadVendor(Bus, Slot, Func);
    if (Vendor == PCI_INVALID_VENDOR) return NULLPTR;
    
    PciDevice* Dev = MemoryAllocate(sizeof(PciDevice));
    MemSet(Dev, 0, sizeof(PciDevice));
    
    Dev->Bus = Bus;
    Dev->Slot = Slot;
    Dev->Function = Func;
    Dev->VendorId = Vendor;
    
    // Reading the main registers
    UINT32 Reg00 = PciRead(Dev, 0x00);
    Dev->DeviceId = (Reg00 >> 16) & 0xFFFF;
    
    UINT32 Reg04 = PciRead(Dev, 0x04);
    Dev->Command = Reg04 & 0xFFFF;
    Dev->Status = (Reg04 >> 16) & 0xFFFF;
    
    UINT32 Reg08 = PciRead(Dev, 0x08);
    Dev->RevisionId = Reg08 & 0xFF;
    Dev->ProgIf = (Reg08 >> 8) & 0xFF;
    Dev->SubClass = (Reg08 >> 16) & 0xFF;
    Dev->ClassCode = (Reg08 >> 24) & 0xFF;
    
    UINT32 Reg0C = PciRead(Dev, 0x0C);
    Dev->CacheLine = Reg0C & 0xFF;
    Dev->LatencyTimer = (Reg0C >> 8) & 0xFF;
    Dev->HeaderType = (Reg0C >> 16) & 0xFF;
    Dev->Bist = (Reg0C >> 24) & 0xFF;
    
    UINT32 Reg3C = PciRead(Dev, 0x3C);
    Dev->InterruptLine = Reg3C & 0xFF;
    Dev->InterruptPin = (Reg3C >> 8) & 0xFF;
    
    // Detecting BAR
    PciDetectBars(Dev);
    
    // Detecting PCIe
    PciDetectPciE(Dev);
    
    // Add to the list
    Dev->Next = PciDevices;
    PciDevices = Dev;
    DeviceCount++;
    
    return Dev;
}

// Scan function
static VOID PciScanFunction(UINT8 Bus, UINT8 Slot, UINT8 Func) {
    PciProbe(Bus, Slot, Func);
    
    // If this is a PCI-to-PCI bridge, scan the secondary bus
    PciDevice* Dev = PciDevices; // Newly added device
    if (Dev->ClassCode == 0x06 && Dev->SubClass == 0x04) {
        UINT8 SecondaryBus = (PciRead(Dev, 0x18) >> 8) & 0xFF;
        if (SecondaryBus != 0) {
            PciScanBus(SecondaryBus);
        }
    }
}

// Scan your device
static VOID PciScanDevice(UINT8 Bus, UINT8 Slot) {
    // Checking function 0
    if (PciReadVendor(Bus, Slot, 0) == PCI_INVALID_VENDOR) return;
    
    PciScanFunction(Bus, Slot, 0);
    
    // Checking multifunction
    PciDevice* Dev = PciDevices; // Just added
    if (Dev->HeaderType & 0x80) {
        for (UINT8 Func = 1; Func < 8; Func++) {
            if (PciReadVendor(Bus, Slot, Func) != PCI_INVALID_VENDOR) {
                PciScanFunction(Bus, Slot, Func);
            }
        }
    }
}

// Bus scan
VOID PciScanBus(UINT8 Bus) {
    for (UINT8 Slot = 0; Slot < 32; Slot++) {
        PciScanDevice(Bus, Slot);
    }
}

// ==================== PUBLIC FUNCTIONS ====================

VOID PciInit(VOID) {
    // Checking the multifunction of the host bridge (0:0:0)
    PciDevice TempDev = {.Bus = 0, .Slot = 0, .Function = 0};
    UINT8 HeaderType = (PciRead(&TempDev, 0x0C) >> 16) & 0xFF;
    
    if (HeaderType & 0x80) {
        //  Multiple host controllers
        for (UINT8 Func = 0; Func < 8; Func++) {
            if (PciReadVendor(0, 0, Func) != PCI_INVALID_VENDOR) {
                PciScanBus(Func);
            }
        }
    } else {
        //  Single host controller
        PciScanBus(0);
    }

    KDriverRegister(KDriverGenerateStruct("PCI", DCL0, TRUE, NULLPTR, NULLPTR));
}

INT PciScan(VOID) {
    // Clearing the old list
    PciDevice* Dev = PciDevices;
    while (Dev) {
        PciDevice* Next = Dev->Next;
        MemoryFree(Dev);
        Dev = Next;
    }
    PciDevices = NULLPTR;
    DeviceCount = 0;
    
    // Let's scan again
    PciInit();
    return DeviceCount;
}

// Search for device
PciDevice* PciFind(UINT16 VendorId, UINT16 DeviceId) {
    PciDevice* Dev = PciDevices;
    while (Dev) {
        if (Dev->VendorId == VendorId && Dev->DeviceId == DeviceId) {
            return Dev;
        }
        Dev = Dev->Next;
    }
    return NULLPTR;
}

// Search by class
PciDevice* PciFindClass(UINT8 ClassCode, UINT8 SubClass) {
    PciDevice* Dev = PciDevices;
    while (Dev) {
        if (Dev->ClassCode == ClassCode && Dev->SubClass == SubClass) {
            return Dev;
        }
        Dev = Dev->Next;
    }
    return NULLPTR;
}

// Turning on the device
VOID PciEnable(PciDevice *Dev) {
    Dev->Command |= (1 << 0) | (1 << 1) | (1 << 2); //  I/O, Memory, Bus Master
    PciWrite(Dev, 0x04, Dev->Command);
}

// Enabling Bus Mastering
VOID PciEnableBusmaster(PciDevice *Dev) {
    Dev->Command |= (1 << 2);
    PciWrite(Dev, 0x04, Dev->Command);
}

// Enable MSI (if supported)
INT PciEnableMsi(PciDevice *Dev, UINT8 Vector, UINT32 ApicId) {
    if (!Dev) RETURN(NO_OBJECT);
    
    UINT8 Offset = PciFindCap(Dev, PCI_CAP_ID_MSI);
    if (!Offset) {
        RETURN(NO_OBJECT);
    }
    
    Dev->MsiCapOffset = Offset;
    
    // Reading control register
    UINT16 MsgControl = PciRead(Dev, Offset + 2) & 0xFFFF;
    BOOL Is64Bit = (MsgControl & PCI_MSI_FLAGS_64BIT) != 0;
    BOOL PerVectorMask = (MsgControl & PCI_MSI_FLAGS_MASKABLE) != 0;
    
    // Calculating offsets
    UINT8 AddrOffset = Offset + 4;
    UINT8 DataOffset = Is64Bit ? Offset + 12 : Offset + 8;
    UINT8 MaskOffset = PerVectorMask ? (Is64Bit ? Offset + 16 : Offset + 12) : 0;
    
    // Set the MSI address (APIC)
    UINT32 MsiAddr = 0xFEE00000 | (ApicId << 24);  //  Physical destination mode
    PciWrite(Dev, AddrOffset, MsiAddr);
    
    if (Is64Bit) {
        PciWrite(Dev, AddrOffset + 4, 0);  //  Upper 32 bits = 0
    }
    
    // Set the data (interrupt vector)
    PciWrite(Dev, DataOffset, Vector);
    
    // If there is masking, we will unmask
    if (PerVectorMask && MaskOffset) {
        UINT32 MaskBits = PciRead(Dev, MaskOffset);
        MaskBits &= ~1;  //  Clear mask for vector 0
        PciWrite(Dev, MaskOffset, MaskBits);
    }
    
    // Turn on MSI
    MsgControl |= PCI_MSI_FLAGS_ENABLE;
    PciWrite(Dev, Offset + 2, MsgControl);
    
    Dev->MsiEnabled = TRUE;
    RETURN(SUCCESS);
}

INT PciEnableMsiX(PciDevice *Dev, INT NumVectors, UINT8 VectorBase, UINT32 ApicId) {
    UINT8 Offset = PciFindCap(Dev, PCI_CAP_ID_MSIX);
    if (!Offset) RETURN(NO_OBJECT);
    
    Dev->MsiXCapOffset = Offset;
    
    UINT16 MsgControl = PciRead(Dev, Offset + 2) & 0xFFFF;
    INT TableSize = (MsgControl & 0x7FF) + 1;
    
    if (NumVectors > TableSize) NumVectors = TableSize;
    
    // Reading the table and PBA
    UINT32 TableBir = PciRead(Dev, Offset + 4);
    UINT32 PbaBir = PciRead(Dev, Offset + 8);
    
    // Turn on MSI-X
    MsgControl |= (1 << 15);  //  Enable MSI-X
    PciWrite(Dev, Offset + 2, MsgControl);
    
    Dev->MsiXEnabled = TRUE;
    RETURN(SUCCESS);
}

// ==================== PCIe SPECIFIC FUNCTIONS ====================

// PCIe Activity Check
BOOL PciEIsActive(PciDevice *Dev) {
    return Dev->IsPciE;
}

// Getting the PCIe Type
const CHAR* PciETypeName(PciDevice *Dev) {
    if (!Dev->IsPciE) return "PCI";
    
    switch (Dev->PciEType) {
        case 0x0: return "Endpoint";
        case 0x1: return "Legacy Endpoint";
        case 0x4: return "Root Port";
        case 0x5: return "Upstream Port";
        case 0x6: return "Downstream Port";
        case 0x7: return "PCIe-to-PCI Bridge";
        case 0x8: return "PCI-to-PCIe Bridge";
        default: return "Unknown";
    }
}

// Getting Speed
UINT8 PciEGetSpeed(PciDevice *Dev) {
    return Dev->IsPciE ? Dev->LinkSpeed : 0;
}

// Getting the width
UINT16 PciEGetWidth(PciDevice *Dev) {
    return Dev->IsPciE ? Dev->LinkWidth : 0;
}

// ==================== INFORMATION FUNCTIONS ====================

// Vendor name
const CHAR* PciVendorName(UINT16 VendorId) {
    switch (VendorId) {
        case 0x8086: return "Intel";
        case 0x1022: return "AMD";
        case 0x10DE: return "NVIDIA";
        case 0x10EC: return "Realtek";
        case 0x1234: return "QEMU";
        case 0x1AF4: return "Red Hat";
        default: return "Unknown";
    }
}

// Class name
const CHAR* PciClassName(UINT8 ClassCode, UINT8 SubClass) {
    switch (ClassCode) {
        case 0x01:
            switch (SubClass) {
                case 0x00: return "SCSI Controller";
                case 0x01: return "IDE Controller";
                case 0x06: return "SATA Controller";
                default: return "Storage Controller";
            }
        case 0x02:
            switch (SubClass) {
                case 0x00: return "Ethernet Controller";
                default: return "Network Controller";
            }
        case 0x03:
            switch (SubClass) {
                case 0x00: return "VGA Controller";
                case 0x02: return "3D Controller";
                default: return "Display Controller";
            }
        case 0x06:
            switch (SubClass) {
                case 0x00: return "Host Bridge";
                case 0x04: return "PCI-to-PCI Bridge";
                default: return "Bridge";
            }
        case 0x0C:
            switch (SubClass) {
                case 0x03: return "USB Controller";
                default: return "Serial Controller";
            }
        default: return "Unknown Device";
    }
}

PciDevice* PciGetFirst(VOID) {
    return PciDevices;
}

PciDevice* PciGetNext(PciDevice *Dev) {
    if (!Dev) return NULLPTR;
    return Dev->Next;
}

UINT32 PciGetCount(VOID) {
    UINT32 Count = 0;
    PciDevice *Dev = PciDevices;
    while (Dev) {
        Count++;
        Dev = Dev->Next;
    }
    return Count;
}
