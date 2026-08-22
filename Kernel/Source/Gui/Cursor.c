#include <Gui/Cursor.h>
#include <Drivers/Ps2/Ps2Mouse.h>
#include <Drivers/Framebuffer.h>
#include <Basecon.h>
#include <Mem/Allocator.h>
#include <Return.h>
#include <Rgb.h>
#include "../Drivers/Framebuffer/Private.h"

// ============================================================================
// CURSOR 16x16 (full arrow with stem)
// ============================================================================

// Pointer mask: 1 — black outline, 0 — inner fill
static const UINT16 CursorData[16] = {
    0b1100000000000000,  // ██░░░░░░░░░
    0b1110000000000000,  // ███░░░░░░░░
    0b1111000000000000,  // ████░░░░░░░
    0b1111100000000000,  // █████░░░░░░
    0b1111110000000000,  // ██████░░░░░
    0b1111111000000000,  // ███████░░░░
    0b1111111100000000,  // ████████░░░
    0b1111111110000000,  // █████████░░
    0b1111111111000000,  // ██████████░
    0b1111111111100000,  // ███████████
    0b1111111100000000,  // ████████░░░
    0b1100111100000000,  // ██░░████░░░
    0b1000111100000000,  // █░░░████░░░
    0b0000011110000000,  // ░░░░░████░░
    0b0000011110000000,  // ░░░░░████░░
    0b0000001110000000   // ░░░░░░███░░
};

// ============================================================================
// CURSOR STATE
// ============================================================================

typedef struct {
    INT32 X;
    INT32 Y;
    INT32 Width;
    INT32 Height;
    BOOL Visible;
    BOOL Initialized;
    UINT32 *Buffer;          // Cursor buffer (width * height)
    UINT32 Color;            // Cursor color (already in pixel format)
    UINT32 OutlineColor;     // Outline color
} MouseCursorState;

static MouseCursorState GCursor = {
    .X = 100,
    .Y = 100,
    .Width = 11,
    .Height = 16,
    .Visible = TRUE,
    .Initialized = FALSE,
    .Buffer = NULLPTR,
    .Color = 0xFFFFFFFF,     // White
    .OutlineColor = 0xFF000000 // Black (ARGB)
};

// ============================================================================
// RENDERING CURSOR TO BUFFER (WITH Stroke) - 16x16
// ============================================================================

static VOID RenderCursor(VOID) {
    if (!GCursor.Visible || !GCursor.Buffer) return;
    
    UINT32 *Buf = GCursor.Buffer;
    INT W = GCursor.Width;
    INT H = GCursor.Height;
    UINT32 Color = GCursor.Color;
    UINT32 OutlineColor = GCursor.OutlineColor;
    
    // Clear buffer
    FastMemSet32(Buf, 0, W * H);
    
    // Drawing a cursor with a stroke
    for (INT Y = 0; Y < H; Y++) {
        UINT16 Row = CursorData[Y];
        for (INT X = 0; X < W; X++) {
            UINT16 Bit = 1 << (15 - X);
            if (Row & Bit) {
                // Checking if a pixel is a border
                BOOL IsBorder = FALSE;
                
                // Checking up
                if (Y > 0) {
                    UINT16 RowUp = CursorData[Y - 1];
                    if (!(RowUp & Bit)) IsBorder = TRUE;
                } else {
                    IsBorder = TRUE; // Upper limit
                }
                
                // Checking down
                if (!IsBorder && Y < H - 1) {
                    UINT16 RowDown = CursorData[Y + 1];
                    if (!(RowDown & Bit)) IsBorder = TRUE;
                }
                
                // Checking left
                if (!IsBorder && X > 0) {
                    UINT16 BitLeft = 1 << (15 - (X - 1));
                    if (!(Row & BitLeft)) IsBorder = TRUE;
                } else if (!IsBorder && X == 0) {
                    IsBorder = TRUE; // Left border
                }
                
                // Checking right
                if (!IsBorder && X < W - 1) {
                    UINT16 BitRight = 1 << (15 - (X + 1));
                    if (!(Row & BitRight)) IsBorder = TRUE;
                } else if (!IsBorder && X == W - 1) {
                    IsBorder = TRUE; // Правая граница
                }
                
                // If it is a border - draw in black, otherwise - use the primary color.
                if (IsBorder) {
                    Buf[Y * W + X] = OutlineColor;
                } else {
                    Buf[Y * W + X] = Color;
                }
            }
        }
    }
}

static VOID UpdateCursor(VOID) {
    if (!GCursor.Initialized) return;
    
    // First we render to the buffer
    RenderCursor();
    
    // Sending a buffer to the cursor layer
    FramebufferSetCursorPosition(GCursor.X, GCursor.Y);
    FramebufferDrawCursor(GCursor.Buffer, GCursor.Width, GCursor.Height);
    
    // Swap
    FramebufferSwapBuffers();
}

// ============================================================================
// MOUSE HANDLER
// ============================================================================

VOID MouseCursorHandler(MouseEvent *Event, VOID *UserData) {
    if (!Event || !GCursor.Initialized) return;
    (VOID)UserData;
    
    // Update position
    GCursor.X += Event->DeltaX;
    GCursor.Y -= Event->DeltaY;
    
    // Constrain to the screen boundaries
    Framebuffer *FB = FramebufferGet();
    if (FB) {
        INT MaxX = (INT)FB->Width - GCursor.Width;
        INT MaxY = (INT)FB->Height - GCursor.Height;
        
        if (GCursor.X < 0) GCursor.X = 0;
        if (GCursor.Y < 0) GCursor.Y = 0;
        if (GCursor.X > MaxX) GCursor.X = MaxX;
        if (GCursor.Y > MaxY) GCursor.Y = MaxY;
    }
    
    // Update screen
    UpdateCursor();
}

INT MouseCursorInit(VOID) {
    if (GCursor.Initialized) return SUCCESS;
    
    // Checking the framebuffer
    Framebuffer *FB = FramebufferGet();
    if (!FB || !FB->Initialized) {
        return NO_OBJECT;
    }
    
    // Checking the mouse
    if (!Ps2MouseIsPresent()) {
        return NO_OBJECT;
    }
    
    // Create layer
    INT Result = FramebufferInitCursorLayer();
    if (Result != 0) {
        return NO_MEMORY;
    }
    
    // Setting the cursor size
    FramebufferSetCursorSize(GCursor.Width, GCursor.Height);
    
    // Allocate buffer
    UINT32 BufferSize = GCursor.Width * GCursor.Height * sizeof(UINT32);
    GCursor.Buffer = (UINT32*)MemoryAllocate(BufferSize);
    if (!GCursor.Buffer) {
        return NO_MEMORY;
    }
    
    // Initialization complete
    GCursor.Initialized = TRUE;
    
    // First render
    UpdateCursor();
    
    // Subscribe to mouse
    Ps2MouseSubscribe(MouseCursorHandler, NULLPTR);
    return SUCCESS;
}

VOID MouseCursorSetPosition(INT32 X, INT32 Y) {
    if (!GCursor.Initialized) return;
    
    Framebuffer *FB = FramebufferGet();
    if (FB) {
        INT MaxX = (INT)FB->Width - GCursor.Width;
        INT MaxY = (INT)FB->Height - GCursor.Height;
        
        if (X < 0) X = 0;
        if (Y < 0) Y = 0;
        if (X > MaxX) X = MaxX;
        if (Y > MaxY) Y = MaxY;
    }
    
    GCursor.X = X;
    GCursor.Y = Y;
    UpdateCursor();
}

VOID MouseCursorGetPosition(INT32 *X, INT32 *Y) {
    if (!GCursor.Initialized || !X || !Y) return;
    *X = GCursor.X;
    *Y = GCursor.Y;
}

VOID MouseCursorShow(VOID) {
    if (!GCursor.Initialized) return;
    if (!GCursor.Visible) {
        GCursor.Visible = TRUE;
        UpdateCursor();
    }
}

VOID MouseCursorHide(VOID) {
    if (!GCursor.Initialized) return;
    if (GCursor.Visible) {
        GCursor.Visible = FALSE;
        // Clear buffer and hide cursor
        if (GCursor.Buffer) {
            FastMemSet32(GCursor.Buffer, 0, GCursor.Width * GCursor.Height);
            FramebufferDrawCursor(GCursor.Buffer, GCursor.Width, GCursor.Height);
            FramebufferSwapBuffers();
        }
    }
}

VOID MouseCursorToggle(VOID) {
    if (GCursor.Visible) {
        MouseCursorHide();
    } else {
        MouseCursorShow();
    }
}

VOID MouseCursorSetColor(FramebufferColor Color) {
    if (!GCursor.Initialized) return;
    
    GCursor.Color = ColorToPixel(Color);
    UpdateCursor();
}

VOID MouseCursorSetOutlineColor(FramebufferColor Color) {
    if (!GCursor.Initialized) return;
    
    GCursor.OutlineColor = ColorToPixel(Color);
    UpdateCursor();
}

VOID MouseCursorSetSize(UINT32 Width, UINT32 Height) {
    if (!GCursor.Initialized) return;
    if (Width == 0 || Height == 0) return;
    
    // Free old buffer
    if (GCursor.Buffer) {
        MemoryFree(GCursor.Buffer);
        GCursor.Buffer = NULLPTR;
    }
    
    // Updating sizes
    GCursor.Width = Width;
    GCursor.Height = Height;
    
    // Allocating a new buffer
    UINT32 BufferSize = Width * Height * sizeof(UINT32);
    GCursor.Buffer = (UINT32*)MemoryAllocate(BufferSize);
    if (!GCursor.Buffer) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Cursor: Out of memory for resize!\n");
        return;
    }
    
    // Update size
    FramebufferSetCursorSize(Width, Height);
    
    // Redrawing
    UpdateCursor();
}
