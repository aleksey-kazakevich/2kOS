#include "../Drivers/Framebuffer/Private.h"
#include <Drivers/Ps2/Ps2Mouse.h>
#include <Drivers/Framebuffer.h>
#include <Mem/Allocator.h>
#include <Gui/Cursor.h>
#include <Time/Timer.h>
#include <Scheduler.h>
#include <SpinLock.h>
#include <Basecon.h>
#include <Return.h>
#include <Rgb.h>

// ============================================================================
// CURSOR 16x16 (full arrow with stem)
// ============================================================================

STATIC const UINT16 CursorData[16] = {
    0b1100000000000000,
    0b1110000000000000,
    0b1111000000000000,
    0b1111100000000000,
    0b1111110000000000,
    0b1111111000000000,
    0b1111111100000000,
    0b1111111110000000,
    0b1111111111000000,
    0b1111111111100000,
    0b1111111100000000,
    0b1100111100000000,
    0b1000111100000000,
    0b0000011110000000,
    0b0000011110000000,
    0b0000001110000000
};

// ============================================================================
// CURSOR STATE
// ============================================================================

typedef struct {
    INT32 X;
    INT32 Y;
    INT32 Width;
    INT32 Height;
    INT32 HotX;
    INT32 HotY;
    BOOL Visible;
    BOOL Initialized;
    UINT32 *Buffer;
    UINT32 Color;
    UINT32 OutlineColor;
    volatile BOOL Dirty;
    volatile BOOL Running;
    volatile BOOL Blocked;
    INT32 PrevDrawX;
    INT32 PrevDrawY;
    UINT32 PrevW;
    UINT32 PrevH;
    BOOL PrevValid;
    volatile BOOL FirstMove;
    SpinLock Lock;
} MouseCursorState;

STATIC MouseCursorState GCursor = {
    .X = 100,
    .Y = 100,
    .Width = 11,
    .Height = 16,
    .HotX = 0,
    .HotY = 0,
    .Visible = FALSE,
    .Initialized = FALSE,
    .Buffer = NULLPTR,
    .Color = 0xFFFFFFFF,
    .OutlineColor = 0xFF000000,
    .PrevDrawX = 0,
    .PrevDrawY = 0,
    .PrevW = 0,
    .PrevH = 0,
    .FirstMove = TRUE
};

STATIC Task *GCursorTask = NULLPTR;

// ============================================================================
// RENDER (outside the lock, writes to GCursor.Buffer)
// ============================================================================

STATIC VOID RenderCursorTo(UINT32 *Buf, INT W, INT H,
                           UINT32 Color, UINT32 OutlineColor) {
    if (!Buf || W <= 0 || H <= 0) return;

    FastMemSet32(Buf, 0, (UINT32)(W * H));

    for (INT Y = 0; Y < H && Y < 16; Y++) {
        UINT16 Row = CursorData[Y];
        for (INT X = 0; X < W && X < 16; X++) {
            UINT16 Bit = 1 << (15 - X);
            if (!(Row & Bit)) continue;

            BOOL IsBorder = FALSE;

            if (Y > 0) {
                UINT16 RowUp = CursorData[Y - 1];
                if (!(RowUp & Bit)) IsBorder = TRUE;
            } else {
                IsBorder = TRUE;
            }

            if (!IsBorder && Y < H - 1) {
                UINT16 RowDown = CursorData[Y + 1];
                if (!(RowDown & Bit)) IsBorder = TRUE;
            }

            if (!IsBorder && X > 0) {
                UINT16 BitLeft = 1 << (15 - (X - 1));
                if (!(Row & BitLeft)) IsBorder = TRUE;
            } else if (!IsBorder && X == 0) {
                IsBorder = TRUE;
            }

            if (!IsBorder && X < W - 1) {
                UINT16 BitRight = 1 << (15 - (X + 1));
                if (!(Row & BitRight)) IsBorder = TRUE;
            } else if (!IsBorder && X == W - 1) {
                IsBorder = TRUE;
            }

            Buf[Y * W + X] = IsBorder ? OutlineColor : Color;
        }
    }
}

// ============================================================================
// UPDATE
//
// IMPORTANT: kcursor is a thread, but it can be preempted by the PS/2 IRQ
// (MouseCursorHandler). Therefore GCursor.Lock is acquired via *_Irq —
// otherwise the PS/2 IRQ could spin forever in while(lock) until kcursor
// releases the lock, and kcursor cannot continue because it is in the IRQ.
// ============================================================================

STATIC VOID UpdateCursor(VOID) {
    if (!GCursor.Initialized) return;

    UINTPTR Flags = SpinLockAcquireIrq(&GCursor.Lock);

    if (!GCursor.Dirty) {
        SpinLockReleaseIrq(&GCursor.Lock, Flags);
        return;
    }

    // Snapshot under the lock
    INT32 X = GCursor.X;
    INT32 Y = GCursor.Y;
    INT32 W = GCursor.Width;
    INT32 H = GCursor.Height;
    INT32 HotX = GCursor.HotX;
    INT32 HotY = GCursor.HotY;
    BOOL Visible = GCursor.Visible;
    UINT32 *Buffer = GCursor.Buffer;
    UINT32 Color = GCursor.Color;
    UINT32 OutlineColor = GCursor.OutlineColor;

    INT32 PrevX = GCursor.PrevDrawX;
    INT32 PrevY = GCursor.PrevDrawY;
    UINT32 PrevW = GCursor.PrevW;
    UINT32 PrevH = GCursor.PrevH;
    BOOL PrevValid = GCursor.PrevValid;

    GCursor.Dirty = FALSE;

    SpinLockReleaseIrq(&GCursor.Lock, Flags);

    if (!Buffer) return;

    if (!Visible) {
        // Hidden: erase the previous rectangle
        if (PrevValid) {
            FramebufferDrawCursor(0, 0, 0, 0,
                                  PrevX, PrevY, PrevW, PrevH,
                                  TRUE, NULLPTR);
            FramebufferSwapBuffers();

            Flags = SpinLockAcquireIrq(&GCursor.Lock);
            GCursor.PrevValid = FALSE;
            SpinLockReleaseIrq(&GCursor.Lock, Flags);
        }
        return;
    }

    // Render into GCursor.Buffer. The buffer does not change after init,
    // MouseCursorSetSize is a no-op, so there is no race here.
    RenderCursorTo(Buffer, W, H, Color, OutlineColor);

    INT32 DrawX = X - HotX;
    INT32 DrawY = Y - HotY;

    FramebufferDrawCursor(DrawX, DrawY, (UINT32)W, (UINT32)H,
                          PrevX, PrevY, PrevW, PrevH, PrevValid,
                          Buffer);

    FramebufferSwapBuffers();

    Flags = SpinLockAcquireIrq(&GCursor.Lock);
    GCursor.PrevDrawX = DrawX;
    GCursor.PrevDrawY = DrawY;
    GCursor.PrevW = (UINT32)W;
    GCursor.PrevH = (UINT32)H;
    GCursor.PrevValid = TRUE;
    SpinLockReleaseIrq(&GCursor.Lock, Flags);
}

// ============================================================================
// MOUSE HANDLER (IRQ CONTEXT)
// ============================================================================

VOID MouseCursorHandler(MouseEvent *Event, VOID *UserData) {
    if (!Event || !GCursor.Initialized) return;
    (VOID)UserData;

    // We are already in an IRQ — SpinLockAcquireIrq is safe (it will keep IF=0).
    UINTPTR Flags = SpinLockAcquireIrq(&GCursor.Lock);

    GCursor.X += Event->DeltaX;
    GCursor.Y -= Event->DeltaY;

    Framebuffer *FB = FramebufferGet();
    if (FB) {
        if (GCursor.X < 0) GCursor.X = 0;
        if (GCursor.Y < 0) GCursor.Y = 0;
        if (GCursor.X >= (INT32)FB->Width)  GCursor.X = (INT32)FB->Width  - 1;
        if (GCursor.Y >= (INT32)FB->Height) GCursor.Y = (INT32)FB->Height - 1;
    }

    GCursor.Dirty = TRUE;

    if (GCursor.FirstMove) {
        GCursor.FirstMove = FALSE;
        GCursor.Visible = TRUE;
    }

    BOOL NeedWake = GCursor.Blocked;
    GCursor.Blocked = FALSE;

    SpinLockReleaseIrq(&GCursor.Lock, Flags);

    if (NeedWake && GCursorTask) {
        TaskWakeup(GCursorTask);
    }
}

// ============================================================================
// KCURSOR THREAD
// ============================================================================

STATIC VOID CursorThread(VOID) {
    GCursorTask = GetCurrentTask();

    while (GCursor.Running) {
        UpdateCursor();

        UINTPTR Flags = SpinLockAcquireIrq(&GCursor.Lock);
        BOOL NeedBlock = !GCursor.Dirty;
        if (NeedBlock) {
            GCursor.Blocked = TRUE;
        }
        SpinLockReleaseIrq(&GCursor.Lock, Flags);

        if (NeedBlock) {
            TaskBlock();
        }
    }

    TaskExit(0);
}

// ============================================================================
// INIT
// ============================================================================

INT MouseCursorInit(VOID) {
    if (GCursor.Initialized) return SUCCESS;

    Framebuffer *FB = FramebufferGet();
    if (!FB || !FB->Initialized) return NO_OBJECT;
    if (!Ps2MouseIsPresent())    return NO_OBJECT;

    INT Result = FramebufferInitCursorLayer();
    if (Result != 0) return NO_MEMORY;

    GCursor.X = (INT32)FB->Width / 2;
    GCursor.Y = (INT32)FB->Height / 2;

    UINT32 BufferSize = (UINT32)(GCursor.Width * GCursor.Height) * sizeof(UINT32);
    GCursor.Buffer = (UINT32*)MemoryAllocate(BufferSize);
    if (!GCursor.Buffer) return NO_MEMORY;

    FastMemSet32(GCursor.Buffer, 0,
                 (UINT32)(GCursor.Width * GCursor.Height));

    SpinLockInit(&GCursor.Lock);
    GCursor.Running = TRUE;
    GCursor.Blocked = FALSE;

    GCursor.Initialized = TRUE;

    INT Pid = TaskCreate(CursorThread, KSTACK_SIZE, "kcursor");
    if (Pid < 0) {
        BaseconPrintf(BASECON_TYPE_WARNING,
                      "cursor: failed to start kcursor thread\n");
    }

    Ps2MouseSubscribe(MouseCursorHandler, NULLPTR);
    return SUCCESS;
}

// ============================================================================
// PUBLIC API
// ============================================================================

VOID MouseCursorSetPosition(INT32 X, INT32 Y) {
    if (!GCursor.Initialized) return;

    Framebuffer *FB = FramebufferGet();

    UINTPTR Flags = SpinLockAcquireIrq(&GCursor.Lock);
    if (FB) {
        if (X < 0) X = 0;
        if (Y < 0) Y = 0;
        if (X >= (INT32)FB->Width)  X = (INT32)FB->Width  - 1;
        if (Y >= (INT32)FB->Height) Y = (INT32)FB->Height - 1;
    }
    GCursor.X = X;
    GCursor.Y = Y;
    GCursor.Dirty = TRUE;
    GCursor.FirstMove = FALSE;
    GCursor.Visible = TRUE;
    SpinLockReleaseIrq(&GCursor.Lock, Flags);
}

VOID MouseCursorSetHotSpot(INT32 HotX, INT32 HotY) {
    if (!GCursor.Initialized) return;

    UINTPTR Flags = SpinLockAcquireIrq(&GCursor.Lock);
    if (HotX < 0 || HotX >= GCursor.Width ||
        HotY < 0 || HotY >= GCursor.Height) {
        SpinLockReleaseIrq(&GCursor.Lock, Flags);
        return;
    }
    GCursor.HotX = HotX;
    GCursor.HotY = HotY;
    GCursor.Dirty = TRUE;
    SpinLockReleaseIrq(&GCursor.Lock, Flags);
}

VOID MouseCursorGetHotSpot(INT32 *HotX, INT32 *HotY) {
    if (!GCursor.Initialized || !HotX || !HotY) return;
    UINTPTR Flags = SpinLockAcquireIrq(&GCursor.Lock);
    *HotX = GCursor.HotX;
    *HotY = GCursor.HotY;
    SpinLockReleaseIrq(&GCursor.Lock, Flags);
}

VOID MouseCursorGetPosition(INT32 *X, INT32 *Y) {
    if (!GCursor.Initialized || !X || !Y) return;
    UINTPTR Flags = SpinLockAcquireIrq(&GCursor.Lock);
    *X = GCursor.X;
    *Y = GCursor.Y;
    SpinLockReleaseIrq(&GCursor.Lock, Flags);
}

VOID MouseCursorShow(VOID) {
    if (!GCursor.Initialized) return;

    UINTPTR Flags = SpinLockAcquireIrq(&GCursor.Lock);
    if (!GCursor.Visible) {
        GCursor.Visible = TRUE;
        GCursor.Dirty = TRUE;
    }
    GCursor.FirstMove = FALSE;
    SpinLockReleaseIrq(&GCursor.Lock, Flags);
}

VOID MouseCursorHide(VOID) {
    if (!GCursor.Initialized) return;

    UINTPTR Flags = SpinLockAcquireIrq(&GCursor.Lock);
    if (GCursor.Visible) {
        GCursor.Visible = FALSE;
        GCursor.Dirty = TRUE;
    }
    SpinLockReleaseIrq(&GCursor.Lock, Flags);
}

VOID MouseCursorToggle(VOID) {
    if (!GCursor.Initialized) return;

    UINTPTR Flags = SpinLockAcquireIrq(&GCursor.Lock);
    BOOL WasVisible = GCursor.Visible;
    SpinLockReleaseIrq(&GCursor.Lock, Flags);

    if (WasVisible) MouseCursorHide();
    else            MouseCursorShow();
}

VOID MouseCursorSetColor(FramebufferColor Color) {
    if (!GCursor.Initialized) return;

    UINTPTR Flags = SpinLockAcquireIrq(&GCursor.Lock);
    GCursor.Color = ColorToPixel(Color);
    GCursor.Dirty = TRUE;
    SpinLockReleaseIrq(&GCursor.Lock, Flags);
}

VOID MouseCursorSetOutlineColor(FramebufferColor Color) {
    if (!GCursor.Initialized) return;

    UINTPTR Flags = SpinLockAcquireIrq(&GCursor.Lock);
    GCursor.OutlineColor = ColorToPixel(Color);
    GCursor.Dirty = TRUE;
    SpinLockReleaseIrq(&GCursor.Lock, Flags);
}

VOID MouseCursorSetSize(UINT32 Width, UINT32 Height) {
    (VOID)Width;
    (VOID)Height;
}