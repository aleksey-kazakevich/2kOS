#pragma once

#include <Types.h>

//==================== GENERAL CONSTANTS ====================
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC
#define PCI_INVALID_VENDOR 0xFFFF

#define PCI_CAP_ID_MSI     0x05
#define PCI_CAP_ID_MSIX    0x11

#define PCI_MSI_FLAGS_ENABLE        (1 << 16)
#define PCI_MSI_FLAGS_64BIT         (1 << 15)
#define PCI_MSI_FLAGS_MASKABLE      (1 << 14)
#define PCI_MSI_FLAGS_VECTOR_MASK   0xFFFF

// MSI address
#define PCI_MSI_ADDRESS_APIC_BASE   0xFEE00000

//==================== STRUCTURES ====================

//MSI capability structure
typedef struct {
    UINT8 CapId;           // 0x05
    UINT8 NextPtr;
    UINT16 MsgControl;     // control register
    UINT32 MsgAddr;        // message address
    UINT32 MsgAddrHi;     // message upper address (64-bit)
    UINT16 MsgData;        // message data
    UINT32 MaskBits;       //mask bits (if available)
    UINT32 PendingBits;    // pending bits
} ATTRIBUTE(packed) PciMsi;

//MSI-X capability structure
typedef struct {
    UINT8 CapId;           // 0x11
    UINT8 NextPtr;
    UINT16 MsgControl;     // control register
    UINT32 TableOffset;    // BAR indicator + offset
    UINT32 PbaOffset;      // PBA BAR indicator + offset
} ATTRIBUTE(packed) PciMsiX;

//Unified structure for PCI/PCIe devices
typedef struct PciDevice {
    //Identification
    UINT8 Bus;
    UINT8 Slot;
    UINT8 Function;
    
    //Standard PCI registers
    UINT16 VendorId;
    UINT16 DeviceId;
    UINT16 Command;
    UINT16 Status;
    UINT8 RevisionId;
    UINT8 ProgIf;
    UINT8 SubClass;
    UINT8 ClassCode;
    UINT8 CacheLine;
    UINT8 LatencyTimer;
    UINT8 HeaderType;
    UINT8 Bist;
    
    //BAR registers
    UINT64 Bars[6];
    UINT64 BarSizes[6];
    UINT8 BarTypes[6];  // 0=32-bit, 1=64-bit, 2=I/O
    
    //Interrupts
    UINT8 InterruptLine;
    UINT8 InterruptPin;
    
    //========== PCIe SPECIFIC FIELDS ==========
    BOOL IsPciE;
    
    // PCIe Capability
    UINT8 PciECapOffset;
    UINT8 PciEType;
    UINT8 LinkSpeed;
    UINT8 MaxLinkSpeed;
    UINT16 LinkWidth;
    UINT16 MaxLinkWidth;
    UINT8 PciEVersion;
    
    // MSI/MSI-X
    UINT8 MsiCapOffset;
    UINT8 MsiXCapOffset;
    BOOL MsiEnabled;
    BOOL MsiXEnabled;
    
    // For a linked list
    struct PciDevice* Next;
} PciDevice;

//==================== UNIFIED FUNCTIONS ====================

//Initialization (works for both PCI and PCIe)
VOID PciInit(VOID);

//Read/write configuration
UINT32 PciRead(PciDevice *Dev, UINT8 Offset);
VOID PciWrite(PciDevice *Dev, UINT8 Offset, UINT32 Value);

//Device discovery
INT PciScan(VOID);
PciDevice* PciFind(UINT16 VendorId, UINT16 DeviceId);
PciDevice* PciFindClass(UINT8 ClassCode, UINT8 SubClass);

//Device management
VOID PciEnable(PciDevice *Dev);
VOID PciEnableBusmaster(PciDevice *Dev);
INT PciEnableMsi(PciDevice *Dev, UINT8 Vector, UINT32 ApicId);
INT PciEnableMsiX(PciDevice *Dev, INT NumVectors, UINT8 VectorBase, UINT32 ApicId);

//Information
const CHAR* PciVendorName(UINT16 VendorId);
const CHAR* PciClassName(UINT8 ClassCode, UINT8 SubClass);

//PCIe specific functions (only work if is_pcie == true)
BOOL PciEIsActive(PciDevice *Dev);
const CHAR* PciETypeName(PciDevice *Dev);
UINT8 PciEGetSpeed(PciDevice *Dev);
UINT16 PciEGetWidth(PciDevice *Dev);

UINT8 PciFindCap(PciDevice *Dev, UINT8 CapId);

PciDevice* PciGetFirst(VOID);
PciDevice* PciGetNext(PciDevice *Dev);
UINT32 PciGetCount(VOID);
