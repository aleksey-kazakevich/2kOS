#include <Drivers/Framebuffer.h>
#include <Lib/String.h>
#include <FontAccess.h>
#include <Mem/Paging.h>
#include <Asm/Cpu.h>
#include <Fatal.h>
#include <Rgb.h>
#include <Smp.h>

#define MAX_FRAMES  32

EXTERN(Framebuffer, GFramebuffer);
volatile UINT32 GPanicActive = 0;

STATIC VOID ATTRIBUTE(noreturn) PanicHaltForever(VOID) {
    for (;;) asm volatile ("cli; hlt" ::: "memory");
    __builtin_unreachable();
}

STATIC INLINE VOID PanicPutPixel(UINT32 X, UINT32 Y, UINT32 Pixel) {
    Framebuffer *FB = &GFramebuffer;
    if (!FB->Initialized) return;
    if (X >= FB->Width || Y >= FB->Height) return;
    if (!FB->Layers[0].Buffer) return;

    UINT8 *Base = (UINT8*)FB->Layers[0].Buffer;
    UINT8 *Dst = Base + Y * FB->Pitch + X * FB->BytesPerPixel;

    if (FB->BytesPerPixel == 4) {
        *(UINT32*)Dst = Pixel;
    } else if (FB->BytesPerPixel == 3) {
        Dst[0] = Pixel & 0xFF;
        Dst[1] = (Pixel >> 8) & 0xFF;
        Dst[2] = (Pixel >> 16) & 0xFF;
    } else if (FB->BytesPerPixel == 2) {
        *(UINT16*)Dst = (UINT16)Pixel;
    }
}

STATIC VOID PanicDrawChar(UINT32 X, UINT32 Y, CHAR C, UINT32 Fg, UINT32 Bg) {
    const UINT8 (*Glyph)[16] = FontGetGlyph(C);
    if (!Glyph) return;

    for (UINT8 Row = 0; Row < 16; Row++) {
        UINT8 Bits = (*Glyph)[Row];
        for (UINT8 Col = 0; Col < 8; Col++) {
            UINT32 Pixel = (Bits & (0x80 >> Col)) ? Fg : Bg;
            PanicPutPixel(X + Col, Y + Row, Pixel);
        }
    }
}

STATIC UINT32 PanicX = 0;
STATIC UINT32 PanicY = 0;
STATIC UINT32 PanicFg = 0;
STATIC UINT32 PanicBg = 0;

STATIC VOID PanicClearScreen(UINT32 Bg) {
    Framebuffer *FB = &GFramebuffer;
    if (!FB->Initialized || !FB->Layers[0].Buffer) return;

    UINT8 *Base = (UINT8*)FB->Layers[0].Buffer;
    USIZE TotalBytes = (USIZE)FB->Height * FB->Pitch;

    if (FB->BytesPerPixel == 4) {
        UINT32 *P = (UINT32*)Base;
        USIZE Count = TotalBytes / 4;
        for (USIZE I = 0; I < Count; I++) P[I] = Bg;
    } else if (FB->BytesPerPixel == 3) {
        for (USIZE I = 0; I + 2 < TotalBytes; I += 3) {
            Base[I] = Bg & 0xFF; Base[I+1] = (Bg>>8)&0xFF; Base[I+2] = (Bg>>16)&0xFF;
        }
    } else if (FB->BytesPerPixel == 2) {
        UINT16 *P = (UINT16*)Base;
        USIZE Count = TotalBytes / 2;
        for (USIZE I = 0; I < Count; I++) P[I] = (UINT16)Bg;
    }
    PanicX = 0;
    PanicY = 0;
}

STATIC VOID PanicNewline(VOID) {
    PanicX = 0;
    PanicY += 16;   // FONT_HEIGHT
    if (PanicY + 16 > GFramebuffer.Height) {
        PanicClearScreen(PanicBg);
    }
}

STATIC VOID PanicPutChar(CHAR C) {
    if (C == '\n') { PanicNewline(); return; }
    if (C == '\r') { PanicX = 0; return; }
    if (C == '\t') { PanicX = (PanicX + 8) & ~7u; return; }
    if (C == '\b') {
        if (PanicX >= 8) PanicX -= 8;
        return;
    }

    if (PanicX + 8 > GFramebuffer.Width) PanicNewline();

    PanicDrawChar(PanicX, PanicY, C, PanicFg, PanicBg);
    PanicX += 8;
}

STATIC VOID PanicPutString(const CHAR *S) {
    if (!S) return;
    while (*S) PanicPutChar(*S++);
}

STATIC VOID PanicPutHex(UINT64 V) {
    CHAR Buf[17];
    Buf[16] = '\0';
    const CHAR *H = "0123456789ABCDEF";
    for (INT I = 15; I >= 0; I--) { Buf[I] = H[V & 0xF]; V >>= 4; }
    PanicPutString(Buf);
}

STATIC VOID PanicPutDec(UINT64 V) {
    CHAR Buf[24];
    INT Pos = 23;
    Buf[Pos] = '\0';
    if (V == 0) { Buf[--Pos] = '0'; }
    else while (V) { Buf[--Pos] = '0' + (V % 10); V /= 10; }
    PanicPutString(Buf + Pos);
}

// ============================================================================
// REGISTERS / STACK
// ============================================================================

STATIC UINT64 PanicColorPixel(UINT8 R, UINT8 G, UINT8 B) {
    FramebufferColor C = { R, G, B, 255 };
    return ColorToPixel(C);
}

STATIC VOID PanicUnwindSafe(UINTPTR *Buffer, USIZE MaxFrames, UINTPTR Rip, UINTPTR Rbp) {
    USIZE Count = 0;
    UINT64 *Root = PagingGetKernelRoot();
    if (Count < MaxFrames) Buffer[Count++] = Rip;

    while (Count < MaxFrames && Rbp > 0x1000 && (Rbp & 0x7) == 0) {
        if (!Root) break;
        if (!PagingLookupVirt(Root, Rbp)) break;
        if (!PagingLookupVirt(Root, Rbp + 8)) break;

        UINTPTR NextRip = *(volatile UINTPTR *)(Rbp + 8);
        UINTPTR NextRbp = *(volatile UINTPTR *)(Rbp);

        if (NextRip == 0) break;
        Buffer[Count++] = NextRip;
        if (NextRbp == Rbp) break;
        Rbp = NextRbp;
    }
    while (Count < MaxFrames) Buffer[Count++] = 0;
}

// ============================================================================
// PANIC SHOW
// ============================================================================

STATIC VOID PanicShow(const CHAR *Title,
                      const CHAR *File, INT Line, const CHAR *Func,
                      InterruptFrame *Frame,
                      UINTPTR FallbackRip) {
    LocalInterruptsDisable();

    PanicBg = PanicColorPixel(0, 0, 0);
    PanicFg = PanicColorPixel(200, 200, 200);
    PanicClearScreen(PanicBg);

    UINT32 Red   = PanicColorPixel(255, 60, 60);
    UINT32 Gray  = PanicColorPixel(190, 190, 190);

    PanicFg = Red;
    PanicPutString("KERNEL PANIC: ");
    PanicPutString(Title ? Title : "(unknown)");
    PanicPutChar('\n');

    PanicFg = Gray;
    PanicPutString("  at ");
    PanicPutString(File ? File : "?");
    PanicPutChar(':');
    PanicPutDec((UINT64)(INT64)Line);
    PanicPutString("  ");
    PanicPutString(Func ? Func : "?");
    PanicPutChar('\n');

    PanicFg = PanicColorPixel(120, 200, 255);
    PanicPutString("[REG]\n");
    PanicFg = Gray;

    UINTPTR Rip = 0, Rbp = 0, Rsp = 0, Cs = 0, Ss = 0, Rflags = 0, Err = 0;

    if (Frame) {
        Rip = Frame->RIP; Rsp = Frame->RSP; Rbp = Frame->RBP;
        Cs = Frame->CS; Ss = Frame->SS; Rflags = Frame->RFLAGS;
        Err = Frame->ErrorCode;

        PanicPutString("RAX="); PanicPutHex(Frame->RAX);
        PanicPutString(" RBX="); PanicPutHex(Frame->RBX);
        PanicPutString(" RCX="); PanicPutHex(Frame->RCX);
        PanicPutString(" RDX="); PanicPutHex(Frame->RDX);
        PanicPutChar('\n');
        PanicPutString("RSI="); PanicPutHex(Frame->RSI);
        PanicPutString(" RDI="); PanicPutHex(Frame->RDI);
        PanicPutString(" RBP="); PanicPutHex(Frame->RBP);
        PanicPutString(" RSP="); PanicPutHex(Frame->RSP);
        PanicPutChar('\n');
        PanicPutString("R8 ="); PanicPutHex(Frame->R8);
        PanicPutString(" R9 ="); PanicPutHex(Frame->R9);
        PanicPutString(" R10="); PanicPutHex(Frame->R10);
        PanicPutString(" R11="); PanicPutHex(Frame->R11);
        PanicPutChar('\n');
        PanicPutString("R12="); PanicPutHex(Frame->R12);
        PanicPutString(" R13="); PanicPutHex(Frame->R13);
        PanicPutString(" R14="); PanicPutHex(Frame->R14);
        PanicPutString(" R15="); PanicPutHex(Frame->R15);
        PanicPutChar('\n');
        PanicPutString("RIP="); PanicPutHex(Frame->RIP);
        PanicPutString(" FLG="); PanicPutHex(Frame->RFLAGS);
        PanicPutString(" CS ="); PanicPutHex(Frame->CS);
        PanicPutString(" SS ="); PanicPutHex(Frame->SS);
        PanicPutChar('\n');
        if (Frame->ErrorCode != 0xFFFFFFFF) {
            PanicPutString("ERR="); PanicPutHex(Frame->ErrorCode);
            PanicPutChar('\n');
        }
    } else {
        Rip = FallbackRip;
        Rbp = GetRbp();
        Rsp = 0;
        PanicPutString("RIP="); PanicPutHex(FallbackRip);
        PanicPutString(" RBP="); PanicPutHex(GetRbp());
        PanicPutChar('\n');
    }

    // Control regs
    PanicFg = PanicColorPixel(120, 200, 255);
    PanicPutString("[CR]\n");
    PanicFg = Gray;
    PanicPutString("CR0="); PanicPutHex(ReadCr0());
    PanicPutString(" CR2="); PanicPutHex(ReadCr2());
    PanicPutString(" CR3="); PanicPutHex(ReadCr3());
    PanicPutString(" CR4="); PanicPutHex(ReadCr4());
    PanicPutChar('\n');

    // Stack
    PanicFg = PanicColorPixel(120, 200, 255);
    PanicPutString("[STACK]\n");
    PanicFg = Gray;

    UINTPTR Frames[MAX_FRAMES];
    PanicUnwindSafe(Frames, MAX_FRAMES, Rip, Rbp);

    for (UINT32 I = 0; I < MAX_FRAMES; I++) {
        if (Frames[I] == 0) break;
        PanicPutString("  [");
        PanicPutDec(I);
        PanicPutString("] ");
        PanicPutHex(Frames[I]);
        PanicPutChar('\n');
    }

    // Final
    PanicFg = Red;
    PanicPutString("\nSYSTEM HALTED\n");
    PanicFg = Gray;

    Framebuffer *FB = &GFramebuffer;
    if (FB->Initialized && FB->Layers[0].Buffer && FB->VirtAddr) {
        MemCpy(FB->VirtAddr, FB->Layers[0].Buffer,
               (USIZE)FB->Height * FB->Pitch);
    }
}

// ============================================================================
// PUBLIC API
// ============================================================================

VOID FatalWithFrame(const CHAR *Message, InterruptFrame *Frame,
                    const CHAR *File, INT Line, const CHAR *Func) {
    UINT32 Old = __sync_lock_test_and_set(&GPanicActive, 1);
    if (Old) PanicHaltForever();

    GPanicActive = 1;

    StopOtherCpus();

    PanicShow(Message, File, Line, Func, Frame, 0);

    PanicHaltForever();
}

VOID FatalImpl(const CHAR *Message, const CHAR *File, INT Line,
               const CHAR *Func, UINTPTR CallerRip) {
    UINT32 Old = __sync_lock_test_and_set(&GPanicActive, 1);
    if (Old) PanicHaltForever();

    GPanicActive = 1;

    StopOtherCpus();

    PanicShow(Message, File, Line, Func, NULLPTR, CallerRip);

    PanicHaltForever();
}