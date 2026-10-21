#pragma once

#include <Types.h>

// Shutting down the system
VOID PowerShutdown(VOID);

// Rebooting the system
VOID PowerReboot(VOID);

// Check if the completion is in progress
BOOL PowerIsShuttingDown(VOID);