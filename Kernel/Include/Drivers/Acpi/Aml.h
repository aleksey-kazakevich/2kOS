#pragma once

#include <Drivers/Acpi/AcpiTables.h>
#include <Types.h>



// ============================================================================
// Public API
// ============================================================================

// AML initialization from DSDT and SSDT.
// Parses namespace, saves methods, and is ready for execution.
VOID AmlInit(SDTHeader *Dsdt, SDTHeader **Ssdts, INT SsdtCount);

// Execute the method along the path. Returns TRUE and the result is in *Out.
// Path: "\_SB.PCI0._STA" or "_SB.PCI0._STA" or "^_STA".
BOOL AmlExecByPath(const CHAR *Path, UINT64 *Out);

// Universal reading of the whole along the path (Name / Integer / Method).
BOOL AmlGetInteger(const CHAR *Path, UINT64 *Out);

// Universal string reading along the path (Name / String / Method).
BOOL AmlGetString(const CHAR *Path, CHAR *Out, INT MaxLen);

// Check the existence of the object.
BOOL AmlFindPath(const CHAR *Path);

// Object type: "Scope", "Name", "Method", "Device", "Alias",
// "OpRegion", "Field", "Processor", "PowerRes", "ThermalZone", "Unknown".
const CHAR *AmlGetKind(const CHAR *Path);

// ============================================================================
// Package access
// ============================================================================

// The number of items in the Package following the specified path.
// Returns -1 in case of an error.
INT AmlGetPackageCount(const CHAR *Path);

//Read Package[index] as an Integer.
BOOL AmlGetPackageInt(const CHAR *Path, INT Index, UINT64 *Out);

// Read Package[index] as a String.
BOOL AmlGetPackageString(const CHAR *Path, INT Index, CHAR *Out, INT MaxLen);

// ============================================================================
// Sleep states (S1..S5)
// ============================================================================

// Utility: Get SLP_TYP for the specified state.
// Name — "_S5", "_S3", "_S4", etc.
// Returns TRUE if it was possible to calculate.
BOOL AmlGetSleepSlpTyp(const CHAR *Name, UINT8 *OutSlpTyp);

// Utility: get \_S5 (SLP_TYP for S5).
BOOL AmlGetS5SlpTyp(UINT8 *OutSlpTyp);

// Utility: get \_S3 (SLP_TYP for S3).
BOOL AmlGetS3SlpTyp(UINT8 *OutSlpTyp);

// Utility: get \_S4 (SLP_TYP for S4).
BOOL AmlGetS4SlpTyp(UINT8 *OutSlpTyp);

typedef enum {
    AMLV_UNINITIALIZED = 0,
    AMLV_INTEGER,
    AMLV_STRING,
    AMLV_BUFFER,
    AMLV_PACKAGE,
    AMLV_NAMEREF,
} AmlValueKind;

typedef struct AmlName AmlName;

typedef struct AmlValue {
    AmlValueKind Kind;
    UINT64 Integer;
    AmlName *NameRef;
    const UINT8 *Data;
    UINT32 DataLen;
    UINT32 NumElements;
} AmlValue;

#define AMLV_MAKE(k) ((AmlValue){ (k), 0, NULLPTR, NULLPTR, 0, 0 })

// Create Values
AmlValue AmlMakeInteger(UINT64 Val);
AmlValue AmlMakeString(const CHAR *Str);
AmlValue AmlMakeBuffer(const VOID *Data, UINT32 Len);

// Call a method with arguments
BOOL AmlCallMethod(const CHAR *Path, INT ArgCount, AmlValue *Args, AmlValue *Out);