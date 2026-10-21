#include <Drivers/Acpi/Acpi.h>
#include <Drivers/Power.h>
#include <Time/Timer.h>
#include <Scheduler.h>
#include <Basecon.h>
#include <Asm/Cpu.h>

STATIC BOOL GShuttingDown = FALSE;

VOID PowerShutdown(VOID) {
    if (GShuttingDown) return;
    GShuttingDown = TRUE;
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "power: shutting down...\n");
    
    // Switching off
    AcpiShutdown();
    
    // If AcpiShutdown didn't work, we hang ourselves.
    LocalInterruptsDisable();
    while (1) {
        Halt();
    }
}

VOID PowerReboot(VOID) {
    if (GShuttingDown) return;
    GShuttingDown = TRUE;
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "power: rebooting...\n");
    
    // Rebooting
    AcpiReboot();
    
    // If AcpiReboot didn't work, we hang ourselves.
    LocalInterruptsDisable();
    while (1) {
        Halt();
    }
}

BOOL PowerIsShuttingDown(VOID) {
    return GShuttingDown;
}