#pragma once

#include <Types.h>
#include <Drivers/Apic/ApicRegs.h>

// ============================================================================
// Public API
// ============================================================================

// APIC initialization (reads from MSR)
INT ApicInit(VOID);

// Turn on/off
VOID ApicEnable(VOID);
VOID ApicDisable(VOID);

// EOI (End of Interrupt)
VOID ApicEoi(VOID);

// Information
UINT32 ApicGetId(VOID);
UINT32 ApicGetVersion(VOID);
BOOL ApicIsEnabled(VOID);
BOOL ApicIsX2ApicMode(VOID);

// Read/Write Registers
UINT32 ApicReadReg(UINT32 Reg);
VOID ApicWriteReg(UINT32 Reg, UINT32 Val);

// Sending IPI
VOID ApicSendIpi(UINT32 ApicId, UINT32 Vector);
VOID ApicSendBroadcast(UINT32 Vector);
VOID ApicSendInit(UINT32 ApicId);
VOID ApicSendStartup(UINT32 ApicId, UINT32 Vector);

// Formatting a destination for IOAPIC
UINT32 ApicFormatDestination(UINT32 ApicId);

// Проверка поддержки X2APIC
BOOL ApicCpuSupportsX2Apic(VOID);
