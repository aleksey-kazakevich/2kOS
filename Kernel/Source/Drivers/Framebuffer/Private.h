#pragma once

#include <Drivers/Framebuffer.h>
#include <Types.h>

EXTERN(Framebuffer, GFramebuffer);

BOOL ClipCoord(INT32 *X, INT32 *Y);
VOID Swap(INT32 *A, INT32 *B);
EXTERN(VOID, FramebufferFlush(VOID));
VOID FastMemSet32(UINT32 *Dst, UINT32 Value, UINT32 Count);
