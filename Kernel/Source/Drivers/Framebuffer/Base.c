#include <Drivers/Framebuffer.h>
#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Mem/Paging.h>
#include <Lib/Math.h>
#include "Private.h"
#include <KDriver.h>
#include <Asm/Cpu.h>
#include <Return.h>

// ============================================================================
// Configuration
// ============================================================================

#define CACHE_LINE_SIZE 64

// ============================================================================
// Global Variables
// ============================================================================

Framebuffer GFramebuffer = {0};

STATIC BOOL SimdAvailable = FALSE;
STATIC BOOL AvxAvailable = FALSE;
STATIC BOOL Avx2Available = FALSE;

STATIC INLINE UINTPTR FramebufferLockAcquire(VOID) {
    UINTPTR Flags = SaveFlags();
    LocalInterruptsDisable();
    SpinLockAcquire(&GFramebuffer.Lock);
    return Flags;
}

STATIC INLINE VOID FramebufferLockRelease(UINTPTR Flags) {
    SpinLockRelease(&GFramebuffer.Lock);
    RestoreFlags(Flags);
}

// ============================================================================
// Checking CPU features
// ============================================================================

STATIC VOID DetectCpuFeatures(VOID) {
    UINT32 Eax, Ebx, Ecx, Edx;
    
    Cpuid(1, &Eax, &Ebx, &Ecx, &Edx);
    SimdAvailable = (Edx & (1 << 23)) != 0;  // SSE
    SimdAvailable |= (Edx & (1 << 25)) != 0; // SSE2
    
    if (Ecx & (1 << 28)) {
        AvxAvailable = TRUE;
        Cpuid(7, &Eax, &Ebx, &Ecx, &Edx);
        Avx2Available = (Ebx & (1 << 5)) != 0;
    }
}

// ============================================================================
// FAST copy/fill functions
// ============================================================================

STATIC INLINE VOID* FastCopyRep(VOID *Dst, const VOID *Src, USIZE Size) {
    asm volatile (
        "rep movsb"
        : "+D"(Dst), "+S"(Src), "+c"(Size)
        : 
        : "memory"
    );
    return Dst;
}

STATIC VOID FastMemSet32_SSE(UINT32 *Dst, UINT32 Value, UINT32 Count) {
    if (!SimdAvailable) {
        for (UINT32 I = 0; I < Count; I++) Dst[I] = Value;
        return;
    }
    
    UINT64 Value64 = ((UINT64)Value << 32) | Value;
    
    asm volatile (
        "movq %0, %%xmm0\n\t"
        "punpcklqdq %%xmm0, %%xmm0\n\t"
        : : "m"(Value64)
        : "xmm0"
    );
    
    UINT32 Blocks = Count / 4;
    UINT32 Remainder = Count % 4;
    
    for (UINT32 I = 0; I < Blocks; I++) {
        asm volatile (
            "movdqu %%xmm0, (%0)"
            : : "r"(Dst + I * 4)
            : "memory"
        );
    }
    
    for (UINT32 I = Blocks * 4; I < Count; I++) {
        Dst[I] = Value;
    }
}

STATIC VOID FastMemSet32_AVX2(UINT32 *Dst, UINT32 Value, UINT32 Count) {
    if (!Avx2Available) {
        FastMemSet32_SSE(Dst, Value, Count);
        return;
    }
    
    UINT64 Value64 = ((UINT64)Value << 32) | Value;
    UINT64 Vector[4] = {Value64, Value64, Value64, Value64};
    
    asm volatile (
        "vmovdqa (%0), %%ymm0\n\t"
        : : "r"(Vector)
        : "ymm0"
    );
    
    UINT32 Blocks = Count / 8;
    UINT32 Remainder = Count % 8;
    
    for (UINT32 I = 0; I < Blocks; I++) {
        asm volatile (
            "vmovdqa %%ymm0, (%0)\n\t"
            : : "r"(Dst + I * 8)
            : "memory"
        );
    }
    
    for (UINT32 I = Blocks * 8; I < Count; I++) {
        Dst[I] = Value;
    }
    
    asm volatile ("vzeroupper");
}

VOID FastMemSet32(UINT32 *Dst, UINT32 Value, UINT32 Count) {
    if (Count == 0) return;
    
    if (Avx2Available && Count >= 64) {
        FastMemSet32_AVX2(Dst, Value, Count);
        return;
    }
    
    if (SimdAvailable && Count >= 16) {
        FastMemSet32_SSE(Dst, Value, Count);
        return;
    }
    
    for (UINT32 I = 0; I < Count; I++) {
        Dst[I] = Value;
    }
}

VOID FastMemCpy32(UINT32 *Dst, UINT32 *Src, UINT32 Count) {
    if (Count == 0 || Dst == Src) return;
    
    if (Count >= 1024) {
        FastCopyRep(Dst, Src, Count * 4);
        return;
    }
    
    if (SimdAvailable && Count >= 16) {
        UINT32 Blocks = Count / 4;
        UINT32 Remainder = Count % 4;
        
        for (UINT32 I = 0; I < Blocks; I++) {
            asm volatile (
                "movdqu (%1), %%xmm0\n\t"
                "movdqu %%xmm0, (%0)\n\t"
                : : "r"(Dst + I * 4), "r"(Src + I * 4)
                : "memory", "xmm0"
            );
        }
        
        for (UINT32 I = Blocks * 4; I < Count; I++) {
            Dst[I] = Src[I];
        }
        return;
    }
    
    for (UINT32 I = 0; I < Count; I++) {
        Dst[I] = Src[I];
    }
}

// ============================================================================
// Basic functions of a framebuffer
// ============================================================================

UINT32 ColorToPixel(FramebufferColor C) {
    if (GFramebuffer.Format == FB_FORMAT_RGB888) {
        return (C.R << 16) | (C.G << 8) | C.B;
    } else if (GFramebuffer.Format == FB_FORMAT_BGR888) {
        return (C.B << 16) | (C.G << 8) | C.R;
    } else if (GFramebuffer.Format == FB_FORMAT_RGB565) {
        return ((C.R >> 3) << 11) | ((C.G >> 2) << 5) | (C.B >> 3);
    }
    return 0;
}

STATIC INLINE BOOL ClipCoordFast(INT32 *X, INT32 *Y, UINT32 *W, UINT32 *H) {
    if (*X < 0) { *W += *X; *X = 0; }
    if (*Y < 0) { *H += *Y; *Y = 0; }
    if (*X + *W > (INT32)GFramebuffer.Width) *W = GFramebuffer.Width - *X;
    if (*Y + *H > (INT32)GFramebuffer.Height) *H = GFramebuffer.Height - *Y;
    return (*W > 0 && *H > 0);
}

BOOL ClipCoord(INT32 *X, INT32 *Y) {  
    if (GFramebuffer.ClipEnabled) {
        if (*X < GFramebuffer.ClipX1 || *X >= GFramebuffer.ClipX2 ||
            *Y < GFramebuffer.ClipY1 || *Y >= GFramebuffer.ClipY2) {
            return FALSE;
        }
    } else {
        if (*X < 0 || *X >= (INT32)GFramebuffer.Width ||
            *Y < 0 || *Y >= (INT32)GFramebuffer.Height) {
            return FALSE;
        }
    }
    return TRUE;
}

// ============================================================================
// WORKING WITH LAYERS
// ============================================================================

STATIC INT FramebufferAllocateLayer(INT LayerIndex) {
    if (LayerIndex < 0 || LayerIndex >= MAX_LAYERS) return -1;
    
    // Increase the layer counter if necessary
    if (LayerIndex >= (INT)GFramebuffer.LayerCount) {
        GFramebuffer.LayerCount = LayerIndex + 1;
    }
    
    FramebufferLayer *Layer = &GFramebuffer.Layers[LayerIndex];
    
    // If the buffer already exists, free it.
    if (Layer->Buffer) {
        MemoryFree(Layer->Buffer);
        Layer->Buffer = NULLPTR;
    }
    
    // Select a buffer for the layer
    USIZE LayerSize = GFramebuffer.Height * GFramebuffer.Pitch;
    Layer->Buffer = MemoryAllocate(LayerSize);
    if (!Layer->Buffer) return -1;
    
    MemSet(Layer->Buffer, 0, LayerSize);
    Layer->Enabled = TRUE;
    Layer->IsCursorLayer = FALSE;
    Layer->CursorX = 0;
    Layer->CursorY = 0;
    Layer->CursorW = 0;
    Layer->CursorH = 0;
    Layer->Index = LayerIndex;
    
    return 0;
}

INT FramebufferCreateLayer(INT LayerIndex) {
    if (!GFramebuffer.Initialized) return -1;
    if (LayerIndex == 0) return -1;
    UINTPTR Flags = FramebufferLockAcquire();
    INT R = FramebufferAllocateLayer(LayerIndex);
    FramebufferLockRelease(Flags);
    return R;
}

INT FramebufferDeleteLayer(INT LayerIndex) {
    if (LayerIndex < 0 || LayerIndex >= (INT)GFramebuffer.LayerCount) return -1;
    if (LayerIndex == 0) return -1;
    UINTPTR Flags = FramebufferLockAcquire();
    FramebufferLayer *Layer = &GFramebuffer.Layers[LayerIndex];
    if (Layer->Buffer) {
        MemoryFree(Layer->Buffer);
        Layer->Buffer = NULLPTR;
    }
    Layer->Enabled = FALSE;
    Layer->IsCursorLayer = FALSE;
    FramebufferLockRelease(Flags);
    return 0;
}

INT FramebufferEnableLayer(INT LayerIndex) {
    if (LayerIndex < 0 || LayerIndex >= (INT)GFramebuffer.LayerCount) return -1;
    UINTPTR Flags = FramebufferLockAcquire();
    GFramebuffer.Layers[LayerIndex].Enabled = TRUE;
    FramebufferLockRelease(Flags);
    return 0;
}

INT FramebufferDisableLayer(INT LayerIndex) {
    if (LayerIndex < 0 || LayerIndex >= (INT)GFramebuffer.LayerCount) return -1;
    if (LayerIndex == 0) return -1;
    UINTPTR Flags = FramebufferLockAcquire();
    GFramebuffer.Layers[LayerIndex].Enabled = FALSE;
    FramebufferLockRelease(Flags);
    return 0;
}

INT FramebufferGetLayerCount(VOID) {
    return GFramebuffer.LayerCount;
}

INT FramebufferInitCursorLayer(VOID) {
    // Create layer 1 for the cursor
    INT Result = FramebufferCreateLayer(CURSOR_LAYER_INDEX);
    if (Result != 0) return -1;
    
    GFramebuffer.Layers[CURSOR_LAYER_INDEX].IsCursorLayer = TRUE;
    
    return 0;
}

VOID FramebufferDrawCursor(INT32 X, INT32 Y, UINT32 W, UINT32 H,
                           INT32 PrevX, INT32 PrevY,
                           UINT32 PrevW, UINT32 PrevH, BOOL PrevValid,
                           const UINT32 *CursorBuffer) {
    if (CURSOR_LAYER_INDEX >= (INT)GFramebuffer.LayerCount) return;

    UINTPTR Flags = FramebufferLockAcquire();

    FramebufferLayer *Layer = &GFramebuffer.Layers[CURSOR_LAYER_INDEX];
    if (!Layer->Enabled || !Layer->Buffer) {
        FramebufferLockRelease(Flags);
        return;
    }

    UINT32 *Dst = (UINT32*)Layer->Buffer;
    INT32 Width  = (INT32)GFramebuffer.Width;
    INT32 Height = (INT32)GFramebuffer.Height;

    // -----------------------------------------------------------------
    // 1. Clear previous rectangle (with clipping)
    // -----------------------------------------------------------------
    if (PrevValid && PrevW > 0 && PrevH > 0) {
        INT32 PX0 = PrevX;
        INT32 PY0 = PrevY;
        INT32 PX1 = PrevX + (INT32)PrevW;
        INT32 PY1 = PrevY + (INT32)PrevH;

        if (PX0 < 0) PX0 = 0;
        if (PY0 < 0) PY0 = 0;
        if (PX1 > Width)  PX1 = Width;
        if (PY1 > Height) PY1 = Height;

        for (INT32 Row = PY0; Row < PY1; Row++) {
            UINT32 *RowPtr = Dst + Row * Width;
            for (INT32 Col = PX0; Col < PX1; Col++) {
                RowPtr[Col] = 0;
            }
        }
    }

    // -----------------------------------------------------------------
    // 2. Draw new cursor (with clipping), if any
    // -----------------------------------------------------------------
    if (!CursorBuffer || W == 0 || H == 0) {
        FramebufferLockRelease(Flags);
        return;
    }

    INT32 DstX0 = X, DstY0 = Y;
    INT32 DrawW = (INT32)W, DrawH = (INT32)H;
    INT32 SrcX0 = 0, SrcY0 = 0;

    if (DstX0 < 0) { SrcX0 = -DstX0; DrawW -= SrcX0; DstX0 = 0; }
    if (DstY0 < 0) { SrcY0 = -DstY0; DrawH -= SrcY0; DstY0 = 0; }
    if (DstX0 + DrawW > Width)  DrawW = Width  - DstX0;
    if (DstY0 + DrawH > Height) DrawH = Height - DstY0;

    if (DrawW <= 0 || DrawH <= 0) {
        FramebufferLockRelease(Flags);
        return;
    }

    for (INT32 Row = 0; Row < DrawH; Row++) {
        UINT32 *DstRow = Dst + (DstY0 + Row) * Width + DstX0;
        const UINT32 *SrcRow = CursorBuffer + (SrcY0 + Row) * (INT32)W + SrcX0;
        for (INT32 Col = 0; Col < DrawW; Col++) {
            UINT32 Pixel = SrcRow[Col];
            if (Pixel != 0) {
                DstRow[Col] = Pixel;
            }
        }
    }

    FramebufferLockRelease(Flags);
}

// ============================================================================
// DRAWING IN LAYERS
// ============================================================================

VOID PutPixelRawLayerLocked(INT Layer, INT32 X, INT32 Y, UINT32 Pixel) {
    if (Layer < 0 || Layer >= (INT)GFramebuffer.LayerCount) return;
    if (!GFramebuffer.Layers[Layer].Enabled) return;
    if (X < 0 || X >= (INT)GFramebuffer.Width ||
        Y < 0 || Y >= (INT)GFramebuffer.Height) return;

    VOID *Buffer = GFramebuffer.Layers[Layer].Buffer;
    if (!Buffer) return;

    UINT8 *Ptr = (UINT8*)Buffer + Y * GFramebuffer.Pitch +
                 X * GFramebuffer.BytesPerPixel;
    if (GFramebuffer.BytesPerPixel == 4) {
        *(UINT32*)Ptr = Pixel;
    } else if (GFramebuffer.BytesPerPixel == 3) {
        Ptr[0] = Pixel & 0xFF;
        Ptr[1] = (Pixel >> 8) & 0xFF;
        Ptr[2] = (Pixel >> 16) & 0xFF;
    } else if (GFramebuffer.BytesPerPixel == 2) {
        *(UINT16*)Ptr = (UINT16)Pixel;
    }
}

VOID PutPixelRawLayer(INT Layer, INT32 X, INT32 Y, UINT32 Pixel) {
    UINTPTR Flags = FramebufferLockAcquire();
    PutPixelRawLayerLocked(Layer, X, Y, Pixel);
    FramebufferLockRelease(Flags);
}

VOID PutPixelRaw(INT32 X, INT32 Y, UINT32 Pixel) {
    return PutPixelRawLayer(0, X, Y, Pixel);
}

VOID FramebufferPutPixelLayer(INT Layer, INT32 X, INT32 Y, FramebufferColor Color) {
    UINT32 Pixel = ColorToPixel(Color);
    return PutPixelRawLayer(Layer, X, Y, Pixel);
}

VOID FramebufferPutPixel(INT32 X, INT32 Y, FramebufferColor Color) {
    return FramebufferPutPixelLayer(0, X, Y, Color);
}

FramebufferColor FramebufferGetPixel(INT32 X, INT32 Y) {
    FramebufferColor Black = RGB_BLACK;
    if (!ClipCoord(&X, &Y)) return Black;
    
    VOID *Buffer = GFramebuffer.Layers[0].Buffer;
    if (!Buffer) return Black;
    
    UINT8 *Ptr = (UINT8*)Buffer + Y * GFramebuffer.Pitch + X * GFramebuffer.BytesPerPixel;
    UINT32 Pixel = 0;
    if (GFramebuffer.BytesPerPixel == 4) Pixel = *(UINT32*)Ptr;
    else if (GFramebuffer.BytesPerPixel == 3) Pixel = Ptr[0] | (Ptr[1] << 8) | (Ptr[2] << 16);
    else if (GFramebuffer.BytesPerPixel == 2) Pixel = *(UINT16*)Ptr;
    
    if (GFramebuffer.Format == FB_FORMAT_RGB888) {
        return (FramebufferColor){ (Pixel >> 16) & 0xFF, (Pixel >> 8) & 0xFF, Pixel & 0xFF, 255 };
    }
    return Black;
}

// ============================================================================
// FILLING RECTANGLES
// ============================================================================

VOID FramebufferFillRectLayerLocked(INT Layer, INT32 X, INT32 Y, UINT32 W, UINT32 H, FramebufferColor Color) {
    if (Layer < 0 || Layer >= (INT)GFramebuffer.LayerCount) return;
    if (!GFramebuffer.Layers[Layer].Enabled) return;
    if (!ClipCoordFast(&X, &Y, &W, &H)) return;
    
    UINT32 Pixel = ColorToPixel(Color);
    VOID *Buffer = GFramebuffer.Layers[Layer].Buffer;
    if (!Buffer) {
	  return;
    }
    
    UINT8 *DstBase = (UINT8*)Buffer;
    UINT8 BytesPerPixel = GFramebuffer.BytesPerPixel;
    UINT32 PitchBytes = GFramebuffer.Pitch;
    
    if (BytesPerPixel == 4) {
        UINT32 *RowPtr = (UINT32*)(DstBase + Y * PitchBytes + X * 4);
        if (X == 0 && W == GFramebuffer.Width) {
            UINT32 TotalPixels = W * H;
            FastMemSet32(RowPtr, Pixel, TotalPixels);
            return;
        }
        for (UINT32 I = 0; I < H; I++) {
            FastMemSet32(RowPtr, Pixel, W);
            RowPtr = (UINT32*)((UINT8*)RowPtr + PitchBytes);
        }
        return;
    }
    
    if (BytesPerPixel == 3) {
        UINT8 *Row = DstBase + Y * PitchBytes + X * 3;
        UINT8 R = (Pixel >> 16) & 0xFF;
        UINT8 G = (Pixel >> 8) & 0xFF;
        UINT8 B = Pixel & 0xFF;
        
        UINT8 Pattern[12];
        for (INT I = 0; I < 4; I++) {
            Pattern[I * 3 + 0] = R;
            Pattern[I * 3 + 1] = G;
            Pattern[I * 3 + 2] = B;
        }
        
        for (UINT32 I = 0; I < H; I++) {
            UINT32 Blocks = W / 4;
            UINT32 Rem = W % 4;
            UINT8 *Ptr = Row;
            
            for (UINT32 J = 0; J < Blocks; J++) {
                FastCopyRep(Ptr, Pattern, 12);
                Ptr += 12;
            }
            
            for (UINT32 J = 0; J < Rem; J++) {
                Ptr[0] = R;
                Ptr[1] = G;
                Ptr[2] = B;
                Ptr += 3;
            }
            Row += PitchBytes;
        }
        return;
    }
    
    if (BytesPerPixel == 2) {
        UINT16 Pixel16 = (UINT16)Pixel;
        UINT16 *Row = (UINT16*)(DstBase + Y * PitchBytes + X * 2);
        for (UINT32 I = 0; I < H; I++) {
            for (UINT32 J = 0; J < W; J++) {
                Row[J] = Pixel16;
            }
            Row = (UINT16*)((UINT8*)Row + PitchBytes);
        }
        return;
    }
    
    // Slow way
    for (UINT32 I = 0; I < H; I++) {
        UINT8 *Row = DstBase + (Y + I) * PitchBytes + X * BytesPerPixel;
        for (UINT32 J = 0; J < W * BytesPerPixel; J++) {
            Row[J] = ((UINT8*)&Pixel)[J % BytesPerPixel];
        }
    }
}

VOID FramebufferFillRectLayer(INT Layer, INT32 X, INT32 Y,
                               UINT32 W, UINT32 H, FramebufferColor Color) {
    UINTPTR Flags = FramebufferLockAcquire();
    FramebufferFillRectLayerLocked(Layer, X, Y, W, H, Color);
    FramebufferLockRelease(Flags);
}

VOID FramebufferFillRect(INT32 X, INT32 Y, UINT32 W, UINT32 H, FramebufferColor Color) {
    return FramebufferFillRectLayer(0, X, Y, W, H, Color);
}

// ============================================================================
// CLEAN UP
// ============================================================================

STATIC VOID FramebufferClearLayerLocked(INT Layer, FramebufferColor Color) {
    if (Layer < 0 || Layer >= (INT)GFramebuffer.LayerCount) return;
    if (!GFramebuffer.Layers[Layer].Enabled) return;
    FramebufferFillRectLayerLocked(Layer, 0, 0,
                                    GFramebuffer.Width,
                                    GFramebuffer.Height, Color);
}

VOID FramebufferClearLayer(INT Layer, FramebufferColor Color) {
    UINTPTR Flags = FramebufferLockAcquire();
    FramebufferClearLayerLocked(Layer, Color);
    FramebufferLockRelease(Flags);
}

VOID FramebufferClear(FramebufferColor Color) {
    return FramebufferClearLayer(0, Color);
}

VOID FramebufferClearBack(VOID) {
    return FramebufferClear(RGB_BLACK);
}

// ============================================================================
// SWAP BUFFERS - MAIN FUNCTION
// ============================================================================

VOID FramebufferSwapBuffers(VOID) {
    if (!GFramebuffer.Initialized) return;

    UINTPTR Flags = FramebufferLockAcquire();
    
    UINT32 Width = GFramebuffer.Width;
    UINT32 Height = GFramebuffer.Height;
    UINT32 TotalPixels = Width * Height;
    UINT32 *Dst = (UINT32*)GFramebuffer.VirtAddr;
    
    // 1. COPY LAYER 0 (main background)
    UINT32 *Layer0 = (UINT32*)GFramebuffer.Layers[0].Buffer;
    if (Layer0) {
        FastMemCpy32(Dst, Layer0, TotalPixels);
    }
    
    // 2. APPLY THE OTHER LAYERS (except for the cursor)
    for (UINT32 I = 1; I < GFramebuffer.LayerCount; I++) {
        FramebufferLayer *Layer = &GFramebuffer.Layers[I];
        if (!Layer->Enabled || Layer->IsCursorLayer) continue;
        if (!Layer->Buffer) continue;
        
        UINT32 *Src = (UINT32*)Layer->Buffer;
        
        // Apply a layer with transparency (0x00000000 = transparent)
        for (UINT32 Y = 0; Y < Height; Y++) {
            for (UINT32 X = 0; X < Width; X++) {
                UINT32 Pixel = Src[Y * Width + X];
                if (Pixel != 0) {
                    Dst[Y * Width + X] = Pixel;
                }
            }
        }
    }
    
    if (CURSOR_LAYER_INDEX < (INT)GFramebuffer.LayerCount) {
    	FramebufferLayer *CursorLayer = &GFramebuffer.Layers[CURSOR_LAYER_INDEX];
    	if (CursorLayer->Enabled && CursorLayer->Buffer && CursorLayer->IsCursorLayer) {
            UINT32 *Cursor = (UINT32*)CursorLayer->Buffer;
            for (UINT32 Y = 0; Y < Height; Y++) {
            	UINT32 *DstRow = Dst + Y * Width;
            	UINT32 *SrcRow = Cursor + Y * Width;
            	for (UINT32 X = 0; X < Width; X++) {
                    UINT32 Pixel = SrcRow[X];
                    if (Pixel != 0) {
                    	DstRow[X] = Pixel;
                    }
            	}
            }
    	}
    }

    FramebufferLockRelease(Flags);
}

INT FramebufferInit(UINT64 PhysAddr, UINT32 Width, UINT32 Height, UINT32 Pitch, UINT8 BytesPerPixel) {
    if (!PhysAddr || Width == 0 || Height == 0 || BytesPerPixel == 0) RETURN(INCORRECT_VALUE);
    
    DetectCpuFeatures();
    
    // Cleaning the structure
    MemSet(&GFramebuffer, 0, sizeof(Framebuffer));
    
    GFramebuffer.PhysAddr = PhysAddr;
    GFramebuffer.VirtAddr = (VOID*)PhysToVirt(PhysAddr);
    GFramebuffer.Width = Width;
    GFramebuffer.Height = Height;
    GFramebuffer.Pitch = Pitch;
    GFramebuffer.Bpp = BytesPerPixel * 8;
    GFramebuffer.BytesPerPixel = BytesPerPixel;
    
    // Determining the format
    if (BytesPerPixel == 4) GFramebuffer.Format = FB_FORMAT_RGB888;
    else if (BytesPerPixel == 3) GFramebuffer.Format = FB_FORMAT_RGB888;
    else if (BytesPerPixel == 2) GFramebuffer.Format = FB_FORMAT_RGB565;
    else GFramebuffer.Format = FB_FORMAT_UNKNOWN;
    
    GFramebuffer.ClipEnabled = FALSE;
    GFramebuffer.LayerCount = 0;

    SpinLockInit(&GFramebuffer.Lock);
    
    // CREATE LAYER 0 (main)
    INT Result = FramebufferAllocateLayer(0);
    if (Result != 0) RETURN(NO_MEMORY);
    GFramebuffer.Layers[0].IsCursorLayer = FALSE;
    
    GFramebuffer.Initialized = TRUE;
    
    KDriverRegister(KDriverGenerateStruct("Framebuffer", 0, TRUE, NULLPTR, NULLPTR));
    FramebufferClear(RGB_BLACK);

    FramebufferRegisterSysDevice();
    
    RETURN(SUCCESS);
}

FramebufferColor FramebufferRgb(UINT8 R, UINT8 G, UINT8 B) {
    return (FramebufferColor){R, G, B, 255};
}

FramebufferColor FramebufferHex(UINT32 Hex) {
    return (FramebufferColor){ (Hex >> 16) & 0xFF, (Hex >> 8) & 0xFF, Hex & 0xFF, 255 };
}

UINT32 FramebufferGetWidth(VOID) {
    return GFramebuffer.Width;
}

UINT32 FramebufferGetHeight(VOID) {
    return GFramebuffer.Height;
}

BOOL FramebufferIsInitialized(VOID) {
    return GFramebuffer.Initialized;
}

UINT8 FramebufferGetBpp(VOID) {
    return GFramebuffer.Bpp;
}

VOID FramebufferSetClip(INT32 X1, INT32 Y1, INT32 X2, INT32 Y2) {
    UINTPTR Flags = FramebufferLockAcquire();
    GFramebuffer.ClipX1 = (X1 < 0) ? 0 : X1;
    GFramebuffer.ClipY1 = (Y1 < 0) ? 0 : Y1;
    GFramebuffer.ClipX2 = (X2 >= (INT32)GFramebuffer.Width)
                          ? (INT32)GFramebuffer.Width : X2;
    GFramebuffer.ClipY2 = (Y2 >= (INT32)GFramebuffer.Height)
                          ? (INT32)GFramebuffer.Height : Y2;
    GFramebuffer.ClipEnabled = TRUE;
    FramebufferLockRelease(Flags);
}

VOID FramebufferResetClip(VOID) {
    UINTPTR Flags = FramebufferLockAcquire();
    GFramebuffer.ClipX1 = 0;
    GFramebuffer.ClipY1 = 0;
    GFramebuffer.ClipX2 = GFramebuffer.Width;
    GFramebuffer.ClipY2 = GFramebuffer.Height;
    GFramebuffer.ClipEnabled = FALSE;
    FramebufferLockRelease(Flags);
}

VOID FramebufferDisableClip(VOID) {
    UINTPTR Flags = FramebufferLockAcquire();
    GFramebuffer.ClipEnabled = FALSE;
    FramebufferLockRelease(Flags);
}

VOID Swap(INT32 *A, INT32 *B) {
    INT32 T = *A;
    *A = *B;
    *B = T;
}

Framebuffer* FramebufferGet(VOID) {
    return &GFramebuffer;
}

VOID FramebufferDrawRect(INT32 X, INT32 Y, UINT32 W, UINT32 H, FramebufferColor Color) {
    if (W < 2 || H < 2) return;
    FramebufferFillRect(X, Y, W, 1, Color);           // Top
    FramebufferFillRect(X, Y + H - 1, W, 1, Color);   // Bottom
    FramebufferFillRect(X, Y, 1, H, Color);           // Left
    FramebufferFillRect(X + W - 1, Y, 1, H, Color);   // Right
}

UINT32 Rgb565ToPixel(UINT16 Rgb565) {
    // Extract 5-6-5 fields and expand to 8 bits by replicating high bits.
    UINT8 R5 = (UINT8)((Rgb565 >> 11) & 0x1F);
    UINT8 G6 = (UINT8)((Rgb565 >> 5)  & 0x3F);
    UINT8 B5 = (UINT8)( Rgb565        & 0x1F);

    UINT8 R = (UINT8)((R5 << 3) | (R5 >> 2));   // 5 -> 8 bits, full range
    UINT8 G = (UINT8)((G6 << 2) | (G6 >> 4));   // 6 -> 8 bits, full range
    UINT8 B = (UINT8)((B5 << 3) | (B5 >> 2));   // 5 -> 8 bits, full range

    FramebufferColor Color = { R, G, B, 255 };
    return ColorToPixel(Color);
}

STATIC VOID DrawBitmapToBuffer(VOID *Buffer, 
                               UINT32 BufferWidth, UINT32 BufferHeight,
                               UINT32 BufferPitch, UINT32 BytesPerPixel,
                               const VOID *BitmapData,
                               UINT32 BmpWidth, UINT32 BmpHeight,
                               INT32 X, INT32 Y,
                               UINT32 Scale,
                               UINT16 TransparentColor,
                               BOOL UseTransparency) {
    
    if (!Buffer || !BitmapData || BmpWidth == 0 || BmpHeight == 0)
        return;
    
    if (Scale == 0) Scale = 1;
    
    const UINT16 *Src = (const UINT16*)BitmapData;
    UINT32 DestWidth = BmpWidth * Scale;
    UINT32 DestHeight = BmpHeight * Scale;
    
    // Clipping
    if (X < 0) { DestWidth += X; X = 0; }
    if (Y < 0) { DestHeight += Y; Y = 0; }
    if ((UINT32)X + DestWidth > BufferWidth)
        DestWidth = BufferWidth - X;
    if ((UINT32)Y + DestHeight > BufferHeight)
        DestHeight = BufferHeight - Y;
    
    if (DestWidth == 0 || DestHeight == 0)
        return;
    
    UINT8 *DstBase = (UINT8*)Buffer;
    UINT32 Pitch = BufferPitch;
    UINT32 Bpp = BytesPerPixel;
    
    // Fast path: no scaling, no transparency
    if (Scale == 1 && !UseTransparency) {
        BOOL Native565 = (Bpp == 2 &&
                     	 GFramebuffer.Format == FB_FORMAT_RGB565);

    	for (UINT32 Row = 0; Row < DestHeight; Row++) {
            UINT8 *Dst = DstBase + (Y + Row) * Pitch + X * Bpp;
            const UINT16 *SrcRow = Src + Row * BmpWidth;

            if (Native565) {
            	UINT16 *Dst16 = (UINT16 *)Dst;
            	for (UINT32 Col = 0; Col < DestWidth; Col++) {
                    Dst16[Col] = SrcRow[Col];
            	}
            } else {
            	for (UINT32 Col = 0; Col < DestWidth; Col++) {
                    UINT32 Pixel = Rgb565ToPixel(SrcRow[Col]);
                    if (Bpp == 4)      *(UINT32*)Dst = Pixel;
                    else if (Bpp == 3) {
                    	Dst[0] = Pixel & 0xFF;
                    	Dst[1] = (Pixel >> 8) & 0xFF;
                    	Dst[2] = (Pixel >> 16) & 0xFF;
                    } else if (Bpp == 2) {
                    	*(UINT16*)Dst = (UINT16)Pixel;
                    }
                    Dst += Bpp;
            	}
            }
    	}
        return;
    }
    
    // Slow path: scaling or transparency
    for (UINT32 Row = 0; Row < DestHeight; Row++) {
        UINT8 *Dst = DstBase + (Y + Row) * Pitch + X * Bpp;
        UINT32 SrcRow = Row / Scale;
        UINT32 SrcColOffset = 0;
        
        for (UINT32 Col = 0; Col < DestWidth; Col++) {
            UINT32 SrcCol = Col / Scale;
            UINT16 Pixel565 = Src[SrcRow * BmpWidth + SrcCol];
            
            // Transparency check
            if (UseTransparency && Pixel565 == TransparentColor) {
                Dst += Bpp;
                continue;
            }
            
            UINT32 Pixel = Rgb565ToPixel(Pixel565);
            
            if (Bpp == 4) {
                *(UINT32*)Dst = Pixel;
            } else if (Bpp == 3) {
                Dst[0] = Pixel & 0xFF;
                Dst[1] = (Pixel >> 8) & 0xFF;
                Dst[2] = (Pixel >> 16) & 0xFF;
            } else if (Bpp == 2) {
                *(UINT16*)Dst = (UINT16)Pixel;
            }
            Dst += Bpp;
        }
    }
}

VOID FramebufferDrawBitmapTransparentLayer(INT Layer,
                                           const VOID *BitmapData,
                                           UINT32 Width, UINT32 Height,
                                           INT32 X, INT32 Y,
                                           UINT32 Scale,
                                           UINT16 TransparentColor) {
    if (!GFramebuffer.Initialized) return;
    if (Layer < 0 || Layer >= (INT)GFramebuffer.LayerCount) return;
    if (!GFramebuffer.Layers[Layer].Enabled) return;
    if (!BitmapData || Width == 0 || Height == 0) return;
    
    VOID *Buffer = GFramebuffer.Layers[Layer].Buffer;
    if (!Buffer) return;
    
    DrawBitmapToBuffer(Buffer,
                       GFramebuffer.Width,
                       GFramebuffer.Height,
                       GFramebuffer.Pitch,
                       GFramebuffer.BytesPerPixel,
                       BitmapData,
                       Width, Height,
                       X, Y,
                       Scale,
                       TransparentColor,
                       TRUE);
    
    FramebufferSwapBuffers();
}

VOID FramebufferDrawBitmapTransparent(const VOID *BitmapData,
                                      UINT32 Width, UINT32 Height,
                                      INT32 X, INT32 Y,
                                      UINT32 Scale,
                                      UINT16 TransparentColor) {
    FramebufferDrawBitmapTransparentLayer(0, BitmapData, Width, Height,
                                          X, Y, Scale, TransparentColor);
}

VOID FramebufferDrawBitmap(const VOID *BitmapData,
                           UINT32 Width, UINT32 Height,
                           INT32 X, INT32 Y,
                           UINT32 Scale) {
    FramebufferDrawBitmapTransparent(BitmapData, Width, Height, X, Y, Scale, 0xFFFF);
}

STATIC const CHAR *FramebufferFormatToString(FramebufferPixelFormat Format) {
    switch (Format) {
        case FB_FORMAT_RGB888:  return "RGB888";
        case FB_FORMAT_BGR888:  return "BGR888";
        case FB_FORMAT_RGB565:  return "RGB565";
        default:                return "UNKNOWN";
    }
}

SysDevice *FramebufferRegisterSysDevice(VOID) {
    if (!GFramebuffer.Initialized) {
        return NULLPTR;
    }

    // Checking if you are already registered.
    SysDevice *Existing = SysDeviceFind("DISPLAY");
    if (Existing) return Existing;

    SysDevice *Parent = SysDeviceGetRoot();
    if (!Parent) return NULLPTR;

    SysDevice *Display = SysDeviceCreate(Parent,
                                          "DISPLAY",
                                          "Primary framebuffer",
                                          SYSDEV_TYPE_DISPLAY);
    if (!Display) return NULLPTR;

    // Resources

    // Physical MMIO region of framebuffer
    UINT64 FbSize = (UINT64)GFramebuffer.Pitch * GFramebuffer.Height;
    SysDeviceAddResource(Display, SYSDEV_RES_MMIO,
                         GFramebuffer.PhysAddr, FbSize);

    // Properties

    SysDeviceSetPropU32(Display, "width",  GFramebuffer.Width);
    SysDeviceSetPropU32(Display, "height", GFramebuffer.Height);
    SysDeviceSetPropU32(Display, "pitch",  GFramebuffer.Pitch);
    SysDeviceSetPropU32(Display, "bpp",    GFramebuffer.Bpp);
    SysDeviceSetPropU32(Display, "bytes_per_pixel",
                        GFramebuffer.BytesPerPixel);
    SysDeviceSetProp(Display, "format",
                     FramebufferFormatToString(GFramebuffer.Format));
    SysDeviceSetPropHex(Display, "phys_addr", GFramebuffer.PhysAddr);
    SysDeviceSetPropHex(Display, "virt_addr",
                        (UINT64)(UINTPTR)GFramebuffer.VirtAddr);
    SysDeviceSetPropU32(Display, "layers", GFramebuffer.LayerCount);
    SysDeviceSetPropU64(Display, "size_bytes", FbSize);

    // --- Linking to KDriver ---

    KDriver *Drv = KDriverFindByName("Framebuffer");
    if (Drv) {
        SysDeviceBindDriver(Display, Drv);
    } else {
        SysDeviceSetState(Display, SYSDEV_STATE_ENABLED);
    }

    return Display;
}

VOID FramebufferDrawBitmapOpaque(const VOID *BitmapData,
                                 UINT32 Width, UINT32 Height,
                                 INT32 X, INT32 Y, UINT32 Scale) {
    if (!GFramebuffer.Initialized) return;
    if (!BitmapData || Width == 0 || Height == 0) return;

    VOID *Buffer = GFramebuffer.Layers[0].Buffer;
    if (!Buffer) return;

    DrawBitmapToBuffer(Buffer,
                       GFramebuffer.Width, GFramebuffer.Height,
                       GFramebuffer.Pitch, GFramebuffer.BytesPerPixel,
                       BitmapData, Width, Height,
                       X, Y, Scale,
                       0x0000,           /* TransparentColor unused */
                       FALSE);            /* UseTransparency = FALSE */

    FramebufferSwapBuffers();
}
