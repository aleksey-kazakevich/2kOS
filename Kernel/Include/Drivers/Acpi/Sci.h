#pragma once

#include <Types.h>

// Initialize SCI
INT AcpiSciInit(VOID);

// Get GSI for SCI
UINT32 AcpiSciGetGsi(VOID);

// Enable/Disable SCI
VOID AcpiSciEnable(VOID);
VOID AcpiSciDisable(VOID);

// SCI handler (call from ISR)
VOID AcpiSciHandler(VOID);

// Check SCI status
UINT32 AcpiSciGetStatus(VOID);

// Clear SCI status
VOID AcpiSciClearStatus(UINT32 Status);

// Wait next SCI event (block)
INT AcpiSciWaitEvent(UINT32 TimeoutMs);

// Get last SCI event
UINT32 AcpiSciGetLastEvent(VOID);

BOOL AcpiSciIsPowerButtonPressed(VOID);