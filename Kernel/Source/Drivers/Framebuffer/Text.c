#include <Drivers/Framebuffer.h>
#include "Private.h"
#include <FontAccess.h>
#include <Lib/String.h>

//=============================================================================
// Text rendering
//=============================================================================

VOID FramebufferDrawChar(CHAR C, INT32 X, INT32 Y, FramebufferColor Fg, FramebufferColor Bg) {
    const UINT8 (*Glyph)[16] = FontGetGlyph(C);
    if (!Glyph) return;
    
    UINT32 FgPixel = ColorToPixel(Fg);
    UINT32 BgPixel = ColorToPixel(Bg);
    
    for (UINT8 Row = 0; Row < FONT_HEIGHT; Row++) {
        UINT8 Bits = (*Glyph)[Row];
        INT32 Py = Y + (INT32)Row;
        
        for (UINT8 Col = 0; Col < FONT_WIDTH; Col++) {
            INT32 Px = X + (INT32)Col;
            
            if (Bits & (0x80 >> Col)) {
                PutPixelRaw(Px, Py, FgPixel);
            } else {
                PutPixelRaw(Px, Py, BgPixel);
            }
        }
    }
}

VOID FramebufferDrawString(const CHAR *Str, INT32 X, INT32 Y, FramebufferColor Fg, FramebufferColor Bg) {
    if (!Str) return;
    
    INT32 CurX = X;
    INT32 CurY = Y;
    
    while (*Str) {
        if (*Str == '\n') {
            CurX = X;
            CurY += FONT_LINE_HEIGHT;
        } else {
            FramebufferDrawChar(*Str, CurX, CurY, Fg, Bg);
            CurX += FONT_WIDTH;
        }
        Str++;
    }
}

UINT32 FramebufferTextWidth(const CHAR *Str) {
    if (!Str) return 0;
    return (UINT32)StrLen(Str) * FONT_WIDTH;
}

UINT32 FramebufferCharWidth(CHAR C) {
    (VOID)C;
    return FONT_WIDTH;
}