#pragma once

#include <Types.h>
#include <List.h>

// ============================================================================
// IOAPIC Flags
// ============================================================================

#define IOAPIC_FLAG_ACTIVE_LOW      (1 << 0)
#define IOAPIC_FLAG_LEVEL_TRIGGERED (1 << 1)
#define IOAPIC_FLAG_ACTIVE_HIGH     (0 << 0)
#define IOAPIC_FLAG_EDGE_TRIGGERED  (0 << 1)

// ============================================================================
// IOAPIC Device Structure
// ============================================================================

typedef struct {
    UINT32 Gsi;
    UINT32 Flags;
    BOOL Valid;
} IoapicOverride;

typedef struct IoapicDevice {
    ListHead Node;
    UINT32 Id;
    UINT32 Address;
    UINT32 GsiBase;
    UINT32 Version;
    UINT32 MaxRedir;
    volatile VOID *VirtAddr;
    BOOL Enabled;
} IoapicDevice;

// ============================================================================
// Public API
// ============================================================================

// Initialize IOAPIC (gets data from ACPI)
INT IoapicInit(VOID);

// Getting information
UINT32 IoapicGetCount(VOID);
IoapicDevice* IoapicGetDevice(UINT32 Index);
IoapicDevice* IoapicFindForGsi(UINT32 Gsi);

// IRQ management
INT IoapicRedirectIrq(UINT32 Gsi, UINT8 Vector, UINT32 ApicId, UINT32 Flags);
INT IoapicUnredirectIrq(UINT32 Gsi);
VOID IoapicMaskIrq(UINT32 Gsi);
VOID IoapicUnmaskIrq(UINT32 Gsi);
VOID IoapicMaskAll(VOID);
VOID IoapicUnmaskAll(VOID);

// EOI for level-triggered
VOID IoapicEoi(UINT32 Gsi);

// Working with Overrides
INT IoapicProcessOverrides(VOID);
INT IoapicGetOverride(UINT32 Source, UINT32 *Gsi, UINT32 *Flags);
