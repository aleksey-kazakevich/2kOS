#include <Drivers/Irq/Apic/ApicRegs.h>
#include <Drivers/Irq/Apic/Ioapic.h>
#include <Drivers/Irq/Apic/Apic.h>
#include <Drivers/Acpi/Acpi.h>
#include <Io/StdWarn.h>
#include <Io/StdErr.h>
#include <Io/StdOut.h>
#include <Hal/Irq.h>
#include <Return.h>
#include <Idt.h>

STATIC volatile BOOL GHalIrqReady = FALSE;

/* ============================================================================ */
/* Init                                                                        */
/* ============================================================================ */

INT HalIrqInit(VOID) {
    if (GHalIrqReady) RETURN(SUCCESS);

    INT Rc = ApicInit();
    if (Rc != SUCCESS) {
        StdErrPrintf("hal/irq: ApicInit failed (%d)\n", Rc);
        RETURN(Rc);
    }

    ApicEnable();

    Rc = IoapicInit();
    if (Rc != SUCCESS) {
        StdErrPrintf("hal/irq: IoapicInit failed (%d)\n", Rc);
        RETURN(Rc);
    }

    IoapicMaskAll();

    GHalIrqReady = TRUE;
    RETURN(SUCCESS);
}

BOOL HalIrqIsReady(VOID) {
    return GHalIrqReady;
}

/* ============================================================================ */
/* GSI                                                                         */
/* ============================================================================ */

INT HalIrqResolve(UINT32 Irq, UINT32 *GsiOut, UINT32 *FlagsOut) {
    if (!GHalIrqReady) RETURN(NOT_READY);

    UINT32 Gsi   = Irq;
    UINT32 Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;

    /* IoapicGetOverride returns SUCCESS even if no override exists:
     * in that case Gsi = Source and Flags = EDGE | HIGH. */
    INT Rc = IoapicGetOverride(Irq, &Gsi, &Flags);
    if (Rc != SUCCESS) {
        /* Fallback: treat the source as the GSI, edge/high. */
        Gsi   = Irq;
        Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
    }

    /* Convert IOAPIC flags into HAL flags. */
    UINT32 HalFlags = 0;
    if (Flags & IOAPIC_FLAG_ACTIVE_LOW)      HalFlags |= HAL_IRQ_FLAG_ACTIVE_LOW;
    if (Flags & IOAPIC_FLAG_LEVEL_TRIGGERED) HalFlags |= HAL_IRQ_FLAG_LEVEL_TRIGGERED;

    if (GsiOut)   *GsiOut   = Gsi;
    if (FlagsOut) *FlagsOut = HalFlags;
    RETURN(SUCCESS);
}

/* ============================================================================ */
/* Routing                                                                     */
/* ============================================================================ */

INT HalIrqRoute(UINT32 Gsi, UINT8 Vector, UINT32 ApicId, UINT32 Flags) {
    if (!GHalIrqReady) RETURN(NOT_READY);

    /* Translate HAL flags into IOAPIC flags. */
    UINT32 IoFlags = 0;
    if (Flags & HAL_IRQ_FLAG_ACTIVE_LOW)      IoFlags |= IOAPIC_FLAG_ACTIVE_LOW;
    if (Flags & HAL_IRQ_FLAG_LEVEL_TRIGGERED) IoFlags |= IOAPIC_FLAG_LEVEL_TRIGGERED;

    INT Rc = IoapicRedirectIrq(Gsi, Vector, ApicId, IoFlags);
    if (Rc != SUCCESS) RETURN(Rc);

    IoapicUnmaskIrq(Gsi);
    RETURN(SUCCESS);
}

INT HalIrqMask(UINT32 Gsi) {
    if (!GHalIrqReady) RETURN(NOT_READY);
    IoapicMaskIrq(Gsi);
    RETURN(SUCCESS);
}

INT HalIrqUnmask(UINT32 Gsi) {
    if (!GHalIrqReady) RETURN(NOT_READY);
    IoapicUnmaskIrq(Gsi);
    RETURN(SUCCESS);
}

VOID HalIrqMaskAll(VOID) {
    if (GHalIrqReady) IoapicMaskAll();
}

VOID HalIrqEoi(VOID) {
    if (GHalIrqReady) ApicEoi();
}

/* ============================================================================ */
/* Local APIC                                                                  */
/* ============================================================================ */

UINT32 HalIrqGetLocalApicId(VOID) {
    return ApicGetId();
}

BOOL HalIrqIsX2Apic(VOID) {
    return ApicIsX2ApicMode();
}

VOID HalIrqSendIpi(UINT32 ApicId, UINT8 Vector) {
    ApicSendIpi(ApicId, Vector);
}

UINT32 HalIrqFormatDest(UINT32 ApicId) {
    return ApicFormatDestination(ApicId);
}

UINT32 HalIrqReadLocal(UINT32 Reg) {
    return ApicReadReg(Reg);
}

VOID HalIrqWriteLocal(UINT32 Reg, UINT32 Value) {
    ApicWriteReg(Reg, Value);
}

VOID HalIrqEnableLocal(VOID) {
    ApicEnable();
}

VOID HalIrqDisableLocal(VOID) {
    ApicDisable();
}

INT HalIrqGetLocalApicVersion(VOID) {
    return ApicGetVersion();
}

/* ============================================================================ */
/* IDT integration                                                             */
/* ============================================================================ */

INT HalIrqInstallHandler(UINT8 Vector, VOID (*Handler)(), UINT8 Ist) {
    if (!Handler) RETURN(INCORRECT_VALUE);
    IdtSetGate(Vector, Handler, KERNEL_CODE_SEL, IDT_GATE_INT, Ist);
    RETURN(SUCCESS);
}