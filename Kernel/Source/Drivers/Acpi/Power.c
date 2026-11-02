#include <Drivers/Acpi/Acpi.h>
#include <Drivers/Power.h>
#include <Mem/PageCache.h>
#include <Time/Timer.h>
#include <Scheduler.h>
#include <Io/StdOut.h>
#include <Asm/Cpu.h>
#include <Smp.h>

STATIC BOOL GShuttingDown = FALSE;
STATIC BOOL GPcacheReady    = FALSE;

/* Called once, after PCacheInit(). Safe to call multiple times. */
VOID PowerInit(VOID) {
    GPcacheReady = TRUE;
}

STATIC VOID PowerSyncAndStopOthers(VOID) {
    /* Disable local interrupts first: we don't want any new I/O in flight. */
    LocalInterruptsDisable();

    /* Flush dirty data. If PCache is not up yet, this is a no-op. */
    if (GPcacheReady) {
        PCacheFlushAll();
    }

    /* Stop all application processors. They enter an hlt loop after NMI. */
    StopOtherCpus();
}

STATIC VOID ATTRIBUTE(noreturn) PowerHaltLoop(VOID) {
    LocalInterruptsDisable();
    for (;;) Halt();
    __builtin_unreachable();
}

VOID PowerShutdown(VOID) {
    if (__sync_lock_test_and_set(&GShuttingDown, TRUE)) {
        PowerHaltLoop();
    }

    StdOutPrintf("\n");
    StdOutPrintf("power: shutting down...\n");

    PowerSyncAndStopOthers();

    /* Try ACPI S5. If it works, we never return. */
    AcpiShutdown();

    StdOutPrintf("power: ACPI shutdown failed, halting\n");
    PowerHaltLoop();
}

VOID PowerReboot(VOID) {
    if (__sync_lock_test_and_set(&GShuttingDown, TRUE)) {
        PowerHaltLoop();
    }

    StdOutPrintf("\n");
    StdOutPrintf("power: rebooting...\n");

    PowerSyncAndStopOthers();

    /* Try ACPI reset register / keyboard controller / CF9. */
    AcpiReboot();

    StdOutPrintf("power: ACPI reboot failed, halting\n");
    PowerHaltLoop();
}

VOID PowerHalt(VOID) {
    if (__sync_lock_test_and_set(&GShuttingDown, TRUE)) {
        PowerHaltLoop();
    }

    StdOutPrintf("\n");
    StdOutPrintf("power: halting...\n");

    PowerSyncAndStopOthers();

    /* No power-off, no reboot — just stop. */
    StdOutPrintf("power: system halted\n");
    PowerHaltLoop();
}

BOOL PowerIsShuttingDown(VOID) {
    return GShuttingDown;
}