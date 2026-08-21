#pragma once

#include <Types.h>
#include <Drivers/Framebuffer.h>
#include <Drivers/Ps2/Ps2Mouse.h>

// ============================================================================
// CURSOR FUNCTIONS
// ============================================================================

INT MouseCursorInit(VOID);
VOID MouseCursorSetPosition(INT32 X, INT32 Y);
VOID MouseCursorGetPosition(INT32 *X, INT32 *Y);
VOID MouseCursorShow(VOID);
VOID MouseCursorHide(VOID);
VOID MouseCursorToggle(VOID);
VOID MouseCursorSetColor(FramebufferColor Color);
VOID MouseCursorSetSize(UINT32 Width, UINT32 Height);

// Mouse handler
VOID MouseCursorHandler(MouseEvent *Event, VOID *UserData);
