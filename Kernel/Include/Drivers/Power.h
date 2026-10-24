#pragma once

#include <Types.h>

/* Initialise power-management (sync/AP-stop readiness). Call once, after
 * PCacheInit() and before user-space starts. */
VOID PowerInit(VOID);

// Shutting down the system
VOID PowerShutdown(VOID);

// Rebooting the system
VOID PowerReboot(VOID);

// Check if the completion is in progress
BOOL PowerIsShuttingDown(VOID);

/* Halt the system without power-off or reboot. Never returns. */
VOID PowerHalt(VOID);