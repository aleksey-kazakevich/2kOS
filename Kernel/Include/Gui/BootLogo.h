#pragma once

#include <Types.h>

/*
 * BootLogo — a very small splash screen shown while the kernel
 * finishes initializing user-visible subsystems.
 *
 * Behaviour:
 *   - BootLogoShow()    — disables Basecon, draws the 2kOS logo in the
 *                          centre of the screen and the text
 *                          "Preparing system..." just below it.
 *                          No-op if the cmdline flag "nobootlogo" is set.
 *   - BootLogoDestroy() — clears the screen and re-enables Basecon.
 *                          Safe to call even if Show was a no-op.
 *
 * The logo is a 64x64 RGB565 image located at:
 *      Include/Gui/2kOSLogo.h  →  const UINT16 G2kOSLogo[64*64];
 */

/* Show the splash screen. Call after Basecon is initialized. */
VOID BootLogoShow(VOID);

/* Tear down the splash screen and re-enable Basecon.
 * Call BEFORE BaseconSetPrompt / BaseconInputStart. */
VOID BootLogoDestroy(VOID);

/* TRUE while the splash screen is currently displayed. */
BOOL BootLogoIsActive(VOID);