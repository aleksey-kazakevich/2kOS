#include <Drivers/Framebuffer.h>
#include <Gui/BootLogo.h>
#include <Gui/2kOSLogo.h>
#include <Io/StdWarn.h>
#include <Lib/String.h>
#include <CmdLine.h>
#include <Basecon.h>
#include <Rgb.h>

#define BOOTLOGO_TEXT      "Preparing system..."
#define BOOTLOGO_TEXT_GAP  24   /* pixels between logo and text */
#define BOOTLOGO_SCALE     3    /* 64x64 -> 192x192, nice and visible */

STATIC volatile BOOL GBootLogoActive = FALSE;

/* ============================================================================ */
/* Internal helpers                                                            */
/* ============================================================================ */

/*
 * Draw the RGB565 logo at (X, Y), scaled by BOOTLOGO_SCALE.
 * FramebufferDrawBitmapOpaque() is the non-transparent variant: pure-white
 * pixels (0xFFFF) must be drawn, not skipped.
 */
STATIC VOID BootLogoDrawImage(UINT32 X, UINT32 Y) {
    FramebufferDrawBitmapOpaque((const VOID *)TwoKOSLogo,
                                BOOTLOGO_WIDTH, BOOTLOGO_HEIGHT,
                                (INT32)X, (INT32)Y,
                                BOOTLOGO_SCALE);
}

STATIC UINT32 BootLogoTextWidth(VOID) {
    return (UINT32)StrLen(BOOTLOGO_TEXT) * FONT_WIDTH;
}

STATIC VOID BootLogoDrawText(UINT32 X, UINT32 Y,
                             FramebufferColor Fg, FramebufferColor Bg) {
    FramebufferDrawString(BOOTLOGO_TEXT,
                          (INT32)X, (INT32)Y,
                          Fg, Bg);
}

/* ============================================================================ */
/* Public API                                                                  */
/* ============================================================================ */

VOID BootLogoShow(VOID) {
    if (GBootLogoActive) return;

    /* Respect the "nobootlogo" cmdline flag (case-insensitive). */
    if (CmdLineIsFlagSet("nobootlogo") || CmdLineIsFlagSet("NOBOOTLOGO")) {
        return;
    }

    Framebuffer *FB = FramebufferGet();
    if (!FB || !FB->Initialized) {
        return;
    }

    /* Hide the console while the splash is visible. */
    BaseconSetEnabled(FALSE);

    /* Clear the whole screen with black. */
    FramebufferClear(RGB_BLACK);

    /* Effective on-screen logo size after scaling. */
    UINT32 DrawW = BOOTLOGO_WIDTH  * BOOTLOGO_SCALE;
    UINT32 DrawH = BOOTLOGO_HEIGHT * BOOTLOGO_SCALE;

    /* Clamp scale if the screen is too small (e.g. 640x480 with scale=3
     * would produce a 192px logo — still fits, but be defensive). */
    if (DrawW > FB->Width)  DrawW = BOOTLOGO_WIDTH  * (FB->Width  / BOOTLOGO_WIDTH);
    if (DrawH > FB->Height) DrawH = BOOTLOGO_HEIGHT * (FB->Height / BOOTLOGO_HEIGHT);

    /* Centre the logo. */
    UINT32 LogoX = (FB->Width  - DrawW) / 2;
    UINT32 LogoY = (FB->Height - DrawH) / 2;

    /* The text goes a bit below the logo, centred horizontally. */
    UINT32 TextW = BootLogoTextWidth();
    UINT32 TextX = (FB->Width > TextW) ? (FB->Width - TextW) / 2 : 0;
    UINT32 TextY = LogoY + DrawH + BOOTLOGO_TEXT_GAP;

    BootLogoDrawImage(LogoX, LogoY);

    /* Only draw the text if it fits on screen. */
    if (TextY + FONT_HEIGHT <= FB->Height && TextW <= FB->Width) {
        BootLogoDrawText(TextX, TextY, RGB_GRAY, RGB_BLACK);
    }

    /* Push everything to the real framebuffer. */
    FramebufferSwapBuffers();

    GBootLogoActive = TRUE;
}

VOID BootLogoDestroy(VOID) {
    if (!GBootLogoActive) return;

    Framebuffer *FB = FramebufferGet();
    if (FB && FB->Initialized) {
        FramebufferClear(RGB_BLACK);
        FramebufferSwapBuffers();
    }

    GBootLogoActive = FALSE;

    /* Re-enable Basecon. Caller must follow this with
     * BaseconSetPrompt() / BaseconInputStart(). */
    BaseconSetEnabled(TRUE);
    BaseconClear();
}

BOOL BootLogoIsActive(VOID) {
    return GBootLogoActive;
}