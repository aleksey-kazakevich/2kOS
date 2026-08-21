#pragma once

#include <Types.h>
#include <Rgb.h>

typedef enum {
    FB_FORMAT_RGB888,
    FB_FORMAT_BGR888,
    FB_FORMAT_RGB565,
    FB_FORMAT_UNKNOWN
} FramebufferPixelFormat;

typedef struct {
    UINT8 R;
    UINT8 G;
    UINT8 B;
    UINT8 A;
} FramebufferColor;

#define FB_RGB(R, G, B) ((FramebufferColor){(R), (G), (B), 255})
#define FB_RGBA(R, G, B, A) ((FramebufferColor){(R), (G), (B), (A)})

#define FONT_HEIGHT 16
#define FONT_WIDTH 8
#define FONT_LINE_SPACING 0
#define FONT_LINE_HEIGHT (FONT_HEIGHT + FONT_LINE_SPACING)

// ============================================================================
// LAYER SYSTEM
// ============================================================================

#define MAX_LAYERS 8
#define CURSOR_LAYER_INDEX 1  // Layer 1 is always for the cursor

typedef struct FramebufferLayer {
    VOID *Buffer;           // Layer buffer (same size as screen)
    BOOL Enabled;            // Is the layer enabled?
    BOOL IsCursorLayer;      // Special flag for cursor layer
    UINT32 CursorX, CursorY; // Cursor position (if it is a cursor layer)
    UINT32 CursorW, CursorH; // Cursor size
    INT Index;
} FramebufferLayer;

// ============================================================================
// BASIC STRUCTURE
// ============================================================================

typedef struct {
    UINT64 PhysAddr;
    VOID *VirtAddr;
    UINT32 Width;
    UINT32 Height;
    UINT32 Pitch;
    UINT8 Bpp;
    FramebufferPixelFormat Format;
    UINT32 BytesPerPixel;
    
    // Layer system
    FramebufferLayer Layers[MAX_LAYERS];
    UINT32 LayerCount;
    
    BOOL ClipEnabled;
    INT32 ClipX1, ClipY1;
    INT32 ClipX2, ClipY2;
    BOOL Initialized;
} Framebuffer;

typedef struct {
    UINT64 Addr;
    UINT32 Pitch;
    UINT32 Width;
    UINT32 Height;
    UINT8 Bpp;
} FramebufferInfo;

// ============================================================================
// PUBLIC FUNCTIONS
// ============================================================================

INT FramebufferInit(UINT64 PhysAddr, UINT32 Width, UINT32 Height, UINT32 Pitch, UINT8 Bpp);
VOID InitFramebuffer(VOID);
Framebuffer *FramebufferGet(VOID);

// --- Drawing functions (default to layer 0) ---
VOID FramebufferPutPixel(INT32 X, INT32 Y, FramebufferColor Color);
VOID FramebufferPutPixelLayer(INT Layer, INT32 X, INT32 Y, FramebufferColor Color);
FramebufferColor FramebufferGetPixel(INT32 X, INT32 Y);
VOID FramebufferClear(FramebufferColor Color);
VOID FramebufferClearLayer(INT Layer, FramebufferColor Color);
VOID FramebufferFillRect(INT32 X, INT32 Y, UINT32 W, UINT32 H, FramebufferColor Color);
VOID FramebufferFillRectLayer(INT Layer, INT32 X, INT32 Y, UINT32 W, UINT32 H, FramebufferColor Color);
VOID FramebufferDrawRect(INT32 X, INT32 Y, UINT32 W, UINT32 H, FramebufferColor Color);

// --- Layer management ---
INT FramebufferCreateLayer(INT LayerIndex);
INT FramebufferDeleteLayer(INT LayerIndex);
INT FramebufferEnableLayer(INT LayerIndex);
INT FramebufferDisableLayer(INT LayerIndex);
INT FramebufferGetLayerCount(VOID);

// --- Special functions for cursor ---
INT FramebufferInitCursorLayer(VOID);
VOID FramebufferSetCursorPosition(INT32 X, INT32 Y);
VOID FramebufferSetCursorSize(UINT32 W, UINT32 H);
VOID FramebufferDrawCursor(UINT32 *CursorBuffer, UINT32 W, UINT32 H);

// --- Rendering functions ---
VOID FramebufferDrawChar(CHAR C, INT32 X, INT32 Y, FramebufferColor Fg, FramebufferColor Bg);
VOID FramebufferDrawString(const CHAR *Str, INT32 X, INT32 Y, FramebufferColor Fg, FramebufferColor Bg);
UINT32 FramebufferTextWidth(const CHAR *Str);
UINT32 FramebufferCharWidth(CHAR C);

// --- Swap and clipping ---
VOID FramebufferSwapBuffers(VOID);
VOID FramebufferClearBack(VOID);
VOID FramebufferSetClip(INT32 X1, INT32 Y1, INT32 X2, INT32 Y2);
VOID FramebufferResetClip(VOID);
VOID FramebufferDisableClip(VOID);

// --- Information ---
UINT32 FramebufferGetWidth(VOID);
UINT32 FramebufferGetHeight(VOID);
BOOL FramebufferIsInitialized(VOID);
UINT8 FramebufferGetBpp(VOID);

// --- Auxiliary ---
FramebufferColor FramebufferRgb(UINT8 R, UINT8 G, UINT8 B);
FramebufferColor FramebufferHex(UINT32 Hex);
UINT32 ColorToPixel(FramebufferColor C);
VOID PutPixelRaw(INT32 X, INT32 Y, UINT32 Pixel);
VOID PutPixelRawLayer(INT Layer, INT32 X, INT32 Y, UINT32 Pixel);
