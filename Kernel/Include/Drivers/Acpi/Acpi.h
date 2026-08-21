#pragma once

#include <Types.h>
#include "AcpiTables.h"

// ============================================================================
// Processor Info
// ============================================================================

typedef struct {
    UINT32 AcpiProcessorUid;
    UINT32 ApicId;
    BOOL Enabled;
    BOOL X2Apic;
} AcpiProcessor;

// ============================================================================
// IO APIC Info
// ============================================================================

typedef struct {
    UINT32 Address;
    UINT32 GsiBase;
    UINT32 Id;
    UINT32 Version;
    UINT32 MaxRedir;
} AcpiIoApic;

// ============================================================================
// Interrupt Override Info
// ============================================================================

typedef struct {
    UINT8  Bus;
    UINT8  Source;
    UINT32 Gsi;
    UINT16 Flags;
} AcpiIntOverride;

// ============================================================================
// APIC Info (parsed from MADT)
// ============================================================================

typedef struct {
    UINT32 LocalApicAddress;
    BOOL UsesX2Apic;
    
    // Processors
    UINT32 ProcessorCount;
    AcpiProcessor Processors[64];
    
    // IO APIC
    UINT32 IoApicCount;
    AcpiIoApic IoApics[16];
    
    // Interrupt Overrides
    UINT32 IntOverrideCount;
    AcpiIntOverride IntOverrides[16];
} AcpiApicInfo;

// ============================================================================
// Main ACPI Structure
// ============================================================================

typedef struct {
    // Tables
    RSDPV2  *Rsdp;
    RSDT    *Rsdt;
    XSDT    *Xsdt;
    FADT    *Fadt;
    MADT    *Madt;
    HPET    *Hpet;
    MCFG    *Mcfg;
    SDTHeader *Dsdt;
    SDTHeader *Ssdts[16];
    INT SsdtCount;
    
    // State
    BOOL     UseXsdt;
    UINT32   ApicBusFreq;
    
    // Parsed data
    AcpiApicInfo Apic;
} Acpi;

// ============================================================================
// Public API
// ============================================================================

// Initialization
INT AcpiInit(UINT64 RsdpAddr);
INT InitAcpiAndApic(VOID);

// Getting the global structure
Acpi *AcpiGet(VOID);

// Search tables
SDTHeader* AcpiFindTable(const CHAR *Signature);
SDTHeader* AcpiFindTableWithIndex(const CHAR *Signature, INT Index);

// Checksum verification
UINT8 AcpiChecksum(VOID *Table, UINT32 Length);

// Access to parsed data
AcpiApicInfo* AcpiGetApicInfo(VOID);
UINT32 AcpiGetLocalApicAddr(VOID);
UINT32 AcpiGetProcessorCount(VOID);
const AcpiProcessor* AcpiGetProcessor(UINT32 Index);
UINT32 AcpiGetIoApicCount(VOID);
const AcpiIoApic* AcpiGetIoApic(UINT32 Index);
UINT32 AcpiGetOverrideCount(VOID);
const AcpiIntOverride* AcpiGetOverride(UINT32 Index);
INT AcpiGetOverrideForSource(UINT8 Source, AcpiIntOverride *Out);

// Power management (minimal, kernel only)
VOID AcpiReboot(VOID);
VOID AcpiShutdown(VOID);
