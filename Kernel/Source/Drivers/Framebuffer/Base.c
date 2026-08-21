#include <Drivers/Framebuffer.h>
#include "Private.h"
#include <KDriver.h>
#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Asm/Cpu.h>
#include <Return.h>
#include <Lib/Math.h>
#include <Mem/Paging.h>

// ============================================================================
// Configuration
// ============================================================================

#define CACHE_LINE_SIZE 64

// ============================================================================
// Global Variables
// ============================================================================

Framebuffer GFramebuffer = {0};

static BOOL SimdAvailable = FALSE;
static BOOL AvxAvailable = FALSE;
static BOOL Avx2Available = FALSE;

// ============================================================================
// Checking CPU features
// ============================================================================

static VOID DetectCpuFeatures(VOID) {
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

static inline VOID* FastCopyRep(VOID *Dst, const VOID *Src, USIZE Size) {
    asm volatile (
        "rep movsb"
        : "+D"(Dst), "+S"(Src), "+c"(Size)
        : 
        : "memory"
    );
    return Dst;
}

static VOID FastMemSet32_SSE(UINT32 *Dst, UINT32 Value, UINT32 Count) {
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

static VOID FastMemSet32_AVX2(UINT32 *Dst, UINT32 Value, UINT32 Count) {
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

static inline BOOL ClipCoordFast(INT32 *X, INT32 *Y, UINT32 *W, UINT32 *H) {
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

static INT FramebufferAllocateLayer(INT LayerIndex) {
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
    if (LayerIndex == 0) return -1; // Layer 0 already exists
    return FramebufferAllocateLayer(LayerIndex);
}

INT FramebufferDeleteLayer(INT LayerIndex) {
    if (LayerIndex < 0 || LayerIndex >= (INT)GFramebuffer.LayerCount) return -1;
    if (LayerIndex == 0) return -1;
    
    FramebufferLayer *Layer = &GFramebuffer.Layers[LayerIndex];
    if (Layer->Buffer) {
        MemoryFree(Layer->Buffer);
        Layer->Buffer = NULLPTR;
    }
    Layer->Enabled = FALSE;
    Layer->IsCursorLayer = FALSE;
    
    return 0;
}

INT FramebufferEnableLayer(INT LayerIndex) {
    if (LayerIndex < 0 || LayerIndex >= (INT)GFramebuffer.LayerCount) return -1;
    GFramebuffer.Layers[LayerIndex].Enabled = TRUE;
    return 0;
}

INT FramebufferDisableLayer(INT LayerIndex) {
    if (LayerIndex < 0 || LayerIndex >= (INT)GFramebuffer.LayerCount) return -1;
    if (LayerIndex == 0) return -1;
    GFramebuffer.Layers[LayerIndex].Enabled = FALSE;
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

VOID FramebufferSetCursorPosition(INT32 X, INT32 Y) {
    if (CURSOR_LAYER_INDEX >= (INT)GFramebuffer.LayerCount) return;
    if (!GFramebuffer.Layers[CURSOR_LAYER_INDEX].Enabled) return;
    
    FramebufferLayer *Layer = &GFramebuffer.Layers[CURSOR_LAYER_INDEX];
    Layer->CursorX = X;
    Layer->CursorY = Y;
}

VOID FramebufferSetCursorSize(UINT32 W, UINT32 H) {
    if (CURSOR_LAYER_INDEX >= (INT)GFramebuffer.LayerCount) return;
    
    FramebufferLayer *Layer = &GFramebuffer.Layers[CURSOR_LAYER_INDEX];
    Layer->CursorW = W;
    Layer->CursorH = H;
}

VOID FramebufferDrawCursor(UINT32 *CursorBuffer, UINT32 W, UINT32 H) {
    if (CURSOR_LAYER_INDEX >= (INT)GFramebuffer.LayerCount) return;
    
    FramebufferLayer *Layer = &GFramebuffer.Layers[CURSOR_LAYER_INDEX];
    if (!Layer->Enabled || !Layer->Buffer) return;
    
    UINT32 *Dst = (UINT32*)Layer->Buffer;
    UINT32 Width = GFramebuffer.Width;
    UINT32 Height = GFramebuffer.Height;
    
    INT32 OldX = Layer->CursorX;
    INT32 OldY = Layer->CursorY;
    UINT32 OldW = Layer->CursorW;
    UINT32 OldH = Layer->CursorH;
    
    if (OldW == 0) OldW = 32;
    if (OldH == 0) OldH = 32;
    
    // Clearing the old cursor position
    for (UINT32 Y = 0; Y < OldH && (OldY + Y) < Height; Y++) {
        for (UINT32 X = 0; X < OldW && (OldX + X) < Width; X++) {
            if (OldY + Y >= 0 && OldX + X >= 0) {
                Dst[(OldY + Y) * Width + (OldX + X)] = 0;
            }
        }
    }
    
    // Drawing a new cursor
    INT32 CX = Layer->CursorX;
    INT32 CY = Layer->CursorY;
    UINT32 CW = Layer->CursorW;
    UINT32 CH = Layer->CursorH;
    
    if (CW == 0) CW = 32;
    if (CH == 0) CH = 32;
    
    if (CX < 0 || CY < 0) return;
    if (CX + CW > Width) CW = Width - CX;
    if (CY + CH > Height) CH = Height - CY;
    
    for (UINT32 Y = 0; Y < CH; Y++) {
        for (UINT32 X = 0; X < CW; X++) {
            UINT32 Pixel = CursorBuffer[Y * W + X];
            if (Pixel != 0) {
                Dst[(CY + Y) * Width + (CX + X)] = Pixel;
            }
        }
    }
}

// ============================================================================
// DRAWING IN LAYERS
// ============================================================================

VOID PutPixelRawLayer(INT Layer, INT32 X, INT32 Y, UINT32 Pixel) {
    if (Layer < 0 || Layer >= (INT)GFramebuffer.LayerCount) return;
    if (!GFramebuffer.Layers[Layer].Enabled) return;
    if (X < 0 || X >= (INT)GFramebuffer.Width || Y < 0 || Y >= (INT)GFramebuffer.Height) return;
    
    VOID *Buffer = GFramebuffer.Layers[Layer].Buffer;
    if (!Buffer) return;
    
    UINT8 *Ptr = (UINT8*)Buffer + Y * GFramebuffer.Pitch + X * GFramebuffer.BytesPerPixel;
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

VOID FramebufferFillRectLayer(INT Layer, INT32 X, INT32 Y, UINT32 W, UINT32 H, FramebufferColor Color) {
    if (Layer < 0 || Layer >= (INT)GFramebuffer.LayerCount) return;
    if (!GFramebuffer.Layers[Layer].Enabled) return;
    if (!ClipCoordFast(&X, &Y, &W, &H)) return;
    
    UINT32 Pixel = ColorToPixel(Color);
    VOID *Buffer = GFramebuffer.Layers[Layer].Buffer;
    if (!Buffer) return;
    
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

VOID FramebufferFillRect(INT32 X, INT32 Y, UINT32 W, UINT32 H, FramebufferColor Color) {
    return FramebufferFillRectLayer(0, X, Y, W, H, Color);
}

// ============================================================================
// CLEAN UP
// ============================================================================

VOID FramebufferClearLayer(INT Layer, FramebufferColor Color) {
    if (Layer < 0 || Layer >= (INT)GFramebuffer.LayerCount) return;
    if (!GFramebuffer.Layers[Layer].Enabled) return;
    
    FramebufferFillRectLayer(Layer, 0, 0, GFramebuffer.Width, GFramebuffer.Height, Color);
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
        UINT32 CursorW = CursorLayer->CursorW;
        UINT32 CursorH = CursorLayer->CursorH;
        INT32 CX = CursorLayer->CursorX;
        INT32 CY = CursorLayer->CursorY;
        
            if (CursorW == 0) CursorW = 16;
            if (CursorH == 0) CursorH = 16;
        
            // Клиппинг
            if (CX < 0) { CursorW += CX; CX = 0; }
            if (CY < 0) { CursorH += CY; CY = 0; }
            if (CX + CursorW > Width) CursorW = Width - CX;
            if (CY + CursorH > Height) CursorH = Height - CY;
        
            if (CursorW == 0 || CursorH == 0) return;
        
            // MAIN CHANGE: read from the layer with the Width step!
            for (UINT32 Y = 0; Y < CursorH; Y++) {
                UINT32 SrcOffset = (CY + Y) * Width + CX;  // ← шаг Width!
                for (UINT32 X = 0; X < CursorW; X++) {
                    UINT32 Pixel = Cursor[SrcOffset + X];
                    if (Pixel != 0) {
                        Dst[SrcOffset + X] = Pixel;
                    }
                }
            }
        }
    }
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
    
    // CREATE LAYER 0 (main)
    INT Result = FramebufferAllocateLayer(0);
    if (Result != 0) RETURN(NO_MEMORY);
    GFramebuffer.Layers[0].IsCursorLayer = FALSE;
    
    GFramebuffer.Initialized = TRUE;
    
    KDriverRegister(KDriverGenerateStruct("Framebuffer", 0, TRUE, NULLPTR, NULLPTR));
    FramebufferClear(RGB_BLACK);
    
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
    GFramebuffer.ClipX1 = (X1 < 0) ? 0 : X1;
    GFramebuffer.ClipY1 = (Y1 < 0) ? 0 : Y1;
    GFramebuffer.ClipX2 = (X2 >= (INT32)GFramebuffer.Width) ? (INT32)GFramebuffer.Width : X2;
    GFramebuffer.ClipY2 = (Y2 >= (INT32)GFramebuffer.Height) ? (INT32)GFramebuffer.Height : Y2;
    GFramebuffer.ClipEnabled = TRUE;
}

VOID FramebufferResetClip(VOID) {
    GFramebuffer.ClipX1 = 0;
    GFramebuffer.ClipY1 = 0;
    GFramebuffer.ClipX2 = GFramebuffer.Width;
    GFramebuffer.ClipY2 = GFramebuffer.Height;
    GFramebuffer.ClipEnabled = FALSE;
}

VOID FramebufferDisableClip(VOID) {
    GFramebuffer.ClipEnabled = FALSE;
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
    FramebufferFillRect(X, Y, W, 1, Color);           // Верх
    FramebufferFillRect(X, Y + H - 1, W, 1, Color);   // Низ
    FramebufferFillRect(X, Y, 1, H, Color);           // Left
    FramebufferFillRect(X + W - 1, Y, 1, H, Color);   // Right
}
