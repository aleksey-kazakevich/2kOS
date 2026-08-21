#pragma once

#include <Types.h>

/*
 * ACPI signatures
 */
#define ACPI_RSDP_SIGNATURE      "RSD PTR "
#define ACPI_RSDP_V1_SIZE        20
#define ACPI_RSDP_V2_SIZE        36

/*
 * MADT entry types
 */
#define MADT_TYPE_LOCAL_APIC              0
#define MADT_TYPE_IO_APIC                  1
#define MADT_TYPE_INT_SOURCE_OVERRIDE       2
#define MADT_TYPE_NMI_SOURCE                3
#define MADT_TYPE_LOCAL_APIC_NMI            4
#define MADT_TYPE_LOCAL_APIC_ADDR_OVERRIDE  5
#define MADT_TYPE_IO_SAPIC                  6
#define MADT_TYPE_LOCAL_SAPIC               7
#define MADT_TYPE_PLATFORM_INT_SOURCE       8
#define MADT_TYPE_PROCESSOR_LOCAL_X2APIC    9
#define MADT_TYPE_LOCAL_X2APIC_NMI          10

/*
 * ACPI System Descriptor Table Header
 */
typedef struct {
    CHAR     Signature[4];
    UINT32   Length;
    UINT8    Revision;
    UINT8    Checksum;
    CHAR     OemId[6];
    CHAR     OemTableId[8];
    UINT32   OemRevision;
    UINT32   CreatorId;
    UINT32   CreatorRevision;
} ATTRIBUTE(packed) SDTHeader;

/*
 * RSDP (v1 and v2)
 */
typedef struct {
    CHAR     Signature[8];
    UINT8    Checksum;
    CHAR     OemId[6];
    UINT8    Revision;
    UINT32   RsdtAddress;
} ATTRIBUTE(packed) RSDPV1;

typedef struct {
    RSDPV1   V1;
    UINT32   Length;
    UINT64   XsdtAddress;
    UINT8    ExtendedChecksum;
    UINT8    Reserved[3];
} ATTRIBUTE(packed) RSDPV2;

/*
 * RSDT (Root System Description Table)
 */
typedef struct {
    SDTHeader Header;
    UINT32    Entries[];
} ATTRIBUTE(packed) RSDT;

/*
 * XSDT (Extended System Description Table)
 */
typedef struct {
    SDTHeader Header;
    UINT64    Entries[];
} ATTRIBUTE(packed) XSDT;

/*
 * FADT (Fixed ACPI Description Table)
 */
typedef struct {
    SDTHeader Header;
    UINT32    FirmwareCtrl;
    UINT32    Dsdt;
    UINT8     Reserved;
    UINT8     PreferredPmProfile;
    UINT16    SciInt;
    UINT32    SmiCmd;
    UINT8     AcpiEnable;
    UINT8     AcpiDisable;
    UINT8     S4BiosReq;
    UINT8     PstateCnt;
    UINT32    Pm1aEvtBlk;
    UINT32    Pm1bEvtBlk;
    UINT32    Pm1aCntBlk;
    UINT32    Pm1bCntBlk;
    UINT32    Pm2CntBlk;
    UINT32    PmTmrBlk;
    UINT32    Gpe0Blk;
    UINT32    Gpe1Blk;
    UINT8     Pm1EvtLen;
    UINT8     Pm1CntLen;
    UINT8     Pm2CntLen;
    UINT8     PmTmrLen;
    UINT8     Gpe0BlkLen;
    UINT8     Gpe1BlkLen;
    UINT8     Gpe1Base;
    UINT8     CstCnt;
    UINT16    PLvl2Lat;
    UINT16    PLvl3Lat;
    UINT16    FlushSize;
    UINT16    FlushStride;
    UINT8     DutyOffset;
    UINT8     DutyWidth;
    UINT8     DayAlrm;
    UINT8     MonAlrm;
    UINT8     Century;
    UINT16    IapcBootArch;
    UINT8     Reserved2;
    UINT32    Flags;
    UINT8     ResetReg[12];      // Generic Address Structure
    UINT8     ResetValue;
    UINT8     Reserved3[3];
    UINT64    XFirmwareCtrl;
    UINT64    XDsdt;
    UINT8     XPm1aEvtBlk[12];
    UINT8     XPm1bEvtBlk[12];
    UINT8     XPm1aCntBlk[12];
    UINT8     XPm1bCntBlk[12];
    UINT8     XPm2CntBlk[12];
    UINT8     XPmTmrBlk[12];
    UINT8     XGpe0Blk[12];
    UINT8     XGpe1Blk[12];
    UINT8     XGpe0BlkLen;       // ACPI 3.0
    UINT8     XGpe1BlkLen;
    UINT8     XGpe1Base;
    UINT8     Reserved4;         // ACPI 4.0
    UINT32    ApicBusFreq;       // APIC timer frequency in Hz
    UINT32    HpetFreq;          // HPET frequency (if present)
} ATTRIBUTE(packed) FADT;

/*
 * MADT (Multiple APIC Description Table)
 */
typedef struct {
    SDTHeader Header;
    UINT32    LocalApicAddress;
    UINT32    Flags;
} ATTRIBUTE(packed) MADT;

/*
 * MADT Entry Header
 */
typedef struct {
    UINT8 Type;
    UINT8 Length;
} ATTRIBUTE(packed) MADTEntryHeader;

/*
 * MADT Local APIC Entry
 */
typedef struct {
    MADTEntryHeader Header;
    UINT8  AcpiProcessorId;
    UINT8  ApicId;
    UINT32 Flags;
} ATTRIBUTE(packed) MADTLocalApic;

/*
 * MADT Processor Local x2APIC Entry (ACPI 4.0+)
 */
typedef struct {
    MADTEntryHeader Header;
    UINT16 Reserved;
    UINT32 LocalX2ApicId;
    UINT32 Flags;
    UINT32 AcpiProcessorUid;
} ATTRIBUTE(packed) MADTLocalX2Apic;

/*
 * MADT IO APIC Entry
 */
typedef struct {
    MADTEntryHeader Header;
    UINT8  IoApicId;
    UINT8  Reserved;
    UINT32 IoApicAddress;
    UINT32 GlobalSystemInterruptBase;
} ATTRIBUTE(packed) MADTIoApic;

/*
 * MADT Interrupt Source Override
 */
typedef struct {
    MADTEntryHeader Header;
    UINT8  Bus;
    UINT8  Source;
    UINT32 GlobalSystemInterrupt;
    UINT16 Flags;
} ATTRIBUTE(packed) MADTIntSourceOverride;

/*
 * HPET (High Precision Event Timer)
 */
typedef struct {
    SDTHeader Header;
    UINT8     HardwareRevId;
    UINT8     ComparatorCount : 5;
    UINT8     CounterSize     : 1;
    UINT8     Reserved        : 1;
    UINT8     LegacyRoute     : 1;
    UINT16    PciVendorId;
    UINT8     AddressSpaceId;
    UINT8     RegisterBitWidth;
    UINT8     RegisterBitOffset;
    UINT8     Reserved2;
    UINT64    BaseAddress;
    UINT8     HpetNumber;
    UINT16    MinimumTick;
    UINT8     PageProtection;
} ATTRIBUTE(packed) HPET;

#define HPET_REG_CAPABILITIES       0x00
#define HPET_REG_CONFIG             0x10
#define HPET_REG_COUNTER            0xF0
#define HPET_REG_TIMER0_CONFIG      0x100
#define HPET_REG_TIMER0_COMPARATOR  0x108

#define HPET_CONFIG_ENABLE          (1ULL << 0)
#define HPET_CONFIG_LEGACY          (1ULL << 1)

/*
 * MCFG (PCI Express Memory Mapped Configuration)
 */
typedef struct {
    SDTHeader Header;
    UINT64    Reserved;
} ATTRIBUTE(packed) MCFG;

typedef struct {
    UINT64    BaseAddress;
    UINT16    PciSegmentGroup;
    UINT8     StartBus;
    UINT8     EndBus;
    UINT32    Reserved;
} ATTRIBUTE(packed) MCFGEntry;
