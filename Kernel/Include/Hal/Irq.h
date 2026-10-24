#pragma once

#include <Types.h>

/*
 * HAL — Hardware Abstraction Layer: Interrupt Routing
 *
 * Abstracts the interrupt controller (currently APIC/IOAPIC).
 * Drivers must use ONLY this interface, never include Apic.h/Ioapic.h.
 *
 * Concepts:
 *   - GSI (Global System Interrupt) — global line number visible to drivers.
 *     For legacy IRQs 0..15 the GSI may differ due to ACPI overrides;
 *     HAL hides this.
 *   - Vector — IDT vector number (32..255) where the interrupt is delivered.
 */

/* ============================================================================ */
/* Line flags (trigger / polarity)                                             */
/* ============================================================================ */

#define HAL_IRQ_FLAG_ACTIVE_LOW       (1u << 0)
#define HAL_IRQ_FLAG_LEVEL_TRIGGERED  (1u << 1)
#define HAL_IRQ_FLAG_ACTIVE_HIGH      (0u)
#define HAL_IRQ_FLAG_EDGE_TRIGGERED   (0u)

/* ============================================================================ */
/* Lifecycle                                                                   */
/* ============================================================================ */

/*
 * Initialize the interrupt HAL. Must be called ONCE, after ACPI/MADT
 * has been parsed, but BEFORE any driver registers its IRQ.
 * Internally configures LAPIC (xAPIC or x2APIC) and all IOAPICs.
 */
INT  HalIrqInit(VOID);
BOOL HalIrqIsReady(VOID);

/* ============================================================================ */
/* GSI (line numbers)                                                          */
/* ============================================================================ */

/*
 * Translate a hardware IRQ (as seen in PCI InterruptLine or PATA_IRQ)
 * into a GSI, taking ACPI overrides into account.
 * If no override exists, returns `Irq` unchanged.
 * FlagsOut (optional) — trigger/polarity for the IOAPIC.
 */
INT HalIrqResolve(UINT32 Irq, UINT32 *GsiOut, UINT32 *FlagsOut);

/*
 * Route a GSI to a vector, targeting the given CPU (ApicId = 0 means
 * the current BSP). Also installs nothing in IDT — call
 * HalIrqInstallHandler first if needed.
 * Returns SUCCESS or an error code.
 */
INT HalIrqRoute(UINT32 Gsi, UINT8 Vector, UINT32 ApicId, UINT32 Flags);

/* Mask / unmask the line on the IOAPIC. */
INT  HalIrqMask(UINT32 Gsi);
INT  HalIrqUnmask(UINT32 Gsi);
VOID HalIrqMaskAll(VOID);

/* EOI — must be called at the end of every ISR. */
VOID HalIrqEoi(VOID);

/* ============================================================================ */
/* Local APIC                                                                  */
/* ============================================================================ */

UINT32 HalIrqGetLocalApicId(VOID);
INT HalIrqGetLocalApicVersion(VOID);
BOOL   HalIrqIsX2Apic(VOID);

/* Send an IPI to a specific CPU. */
VOID   HalIrqSendIpi(UINT32 ApicId, UINT8 Vector);

/* Format destination for IOAPIC (accounts for x2APIC). */
UINT32 HalIrqFormatDest(UINT32 ApicId);

/* ============================================================================ */
/* Direct access to the local APIC (timer, NMI, etc.)                          */
/* ============================================================================ */

UINT32 HalIrqReadLocal(UINT32 Reg);
VOID   HalIrqWriteLocal(UINT32 Reg, UINT32 Value);

VOID   HalIrqEnableLocal(VOID);
VOID   HalIrqDisableLocal(VOID);

/* ============================================================================ */
/* IDT integration                                                             */
/* ============================================================================ */

/*
 * Install a handler for a vector. Wraps IdtSetGate so drivers
 * don't need to include <Idt.h>.
 * Ist = 0 for a normal interrupt gate.
 */
INT HalIrqInstallHandler(UINT8 Vector, VOID (*Handler)(), UINT8 Ist);