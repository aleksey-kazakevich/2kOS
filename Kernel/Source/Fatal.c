#include <Basecon.h>
#include <Asm/Cpu.h>
#include <Fatal.h>
#include <Rgb.h>

#define MAX_FRAMES  32

STATIC BOOL PanicAlreadyRunning = FALSE;

// ============================================================================
// HELPERS
// ============================================================================

// Печатает 4 регистра в одну строку. Ровные колонки.
STATIC VOID PanicReg4(const CHAR *N0, UINT64 V0,
                      const CHAR *N1, UINT64 V1,
                      const CHAR *N2, UINT64 V2,
                      const CHAR *N3, UINT64 V3) {
    BaseconPrintf(BASECON_TYPE_NORMAL,
        "  %-4s %016llX  %-4s %016llX  %-4s %016llX  %-4s %016llX\n",
        N0, V0, N1, V1, N2, V2, N3, V3);
}

STATIC VOID PanicReg2(const CHAR *N0, UINT64 V0,
                      const CHAR *N1, UINT64 V1) {
    BaseconPrintf(BASECON_TYPE_NORMAL,
        "  %-4s %016llX  %-4s %016llX\n",
        N0, V0, N1, V1);
}

// ============================================================================
// REGISTERS
// ============================================================================

STATIC VOID PanicDumpRegsFromFrame(InterruptFrame *Frame) {
    if (!Frame) return;

    BaseconPrintf(BASECON_TYPE_NORMAL, "[REG]\n");

    PanicReg4("RAX", Frame->RAX, "RBX", Frame->RBX,
              "RCX", Frame->RCX, "RDX", Frame->RDX);
    PanicReg4("RSI", Frame->RSI, "RDI", Frame->RDI,
              "RBP", Frame->RBP, "RSP", Frame->RSP);
    PanicReg4("R8",  Frame->R8,  "R9",  Frame->R9,
              "R10", Frame->R10, "R11", Frame->R11);
    PanicReg4("R12", Frame->R12, "R13", Frame->R13,
              "R14", Frame->R14, "R15", Frame->R15);
    PanicReg4("RIP", Frame->RIP, "FLG", Frame->RFLAGS,
              "CS",  Frame->CS,  "SS",  Frame->SS);

    if (Frame->ErrorCode != 0xFFFFFFFF) {
        PanicReg2("ERR", Frame->ErrorCode, "--", 0);
    }
}

STATIC VOID PanicDumpControlRegs(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "[CR]\n");
    PanicReg4("CR0", ReadCr0(), "CR2", ReadCr2(),
              "CR3", ReadCr3(), "CR4", ReadCr4());
}

// ============================================================================
// STACK UNWIND
// ============================================================================

STATIC USIZE PanicUnwind(UINTPTR *Buffer, USIZE MaxFrames,
                         UINTPTR Rip, UINTPTR Rbp) {
    USIZE Count = 0;

    if (Count < MaxFrames) Buffer[Count++] = Rip;

    while (Count < MaxFrames && Rbp > 0x1000 && (Rbp & 0x7) == 0) {
        UINTPTR NextRip = *(volatile UINTPTR *)(Rbp + 8);
        UINTPTR NextRbp = *(volatile UINTPTR *)(Rbp);

        if (NextRip == 0) break;
        Buffer[Count++] = NextRip;
        if (NextRbp == Rbp) break;
        Rbp = NextRbp;
    }

    return Count;
}

STATIC VOID PanicDumpStack(UINTPTR Rip, UINTPTR Rbp) {
    UINTPTR Frames[MAX_FRAMES];
    USIZE Count = PanicUnwind(Frames, MAX_FRAMES, Rip, Rbp);

    BaseconPrintf(BASECON_TYPE_NORMAL, "[STACK] %u frames\n",
                  (UINT32)Count);

    if (Count == 0) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "  <empty>\n");
        return;
    }

    // Печатаем по 2 адреса в строку
    for (USIZE I = 0; I < Count; I += 2) {
        if (I + 1 < Count) {
            BaseconPrintf(BASECON_TYPE_NORMAL,
                "  [%02u] %016llX  [%02u] %016llX\n",
                (UINT32)I, (UINT64)Frames[I],
                (UINT32)(I + 1), (UINT64)Frames[I + 1]);
        } else {
            BaseconPrintf(BASECON_TYPE_NORMAL,
                "  [%02u] %016llX\n",
                (UINT32)I, (UINT64)Frames[I]);
        }
    }
}

// ============================================================================
// COMMON
// ============================================================================

STATIC VOID PanicShow(const CHAR *Title,
                      const CHAR *File, INT Line, const CHAR *Func,
                      InterruptFrame *Frame,
                      UINTPTR FallbackRip) {
    LocalInterruptsDisable();
    BaseconClear();

    // Заголовок — единственная красная строка
    BaseconSetColors(RGB_RED, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL,
        "KERNEL PANIC: %s\n", Title ? Title : "(unknown)");
    BaseconSetColors(RGB_GRAY, RGB_BLACK);

    // Локация — одной строкой
    BaseconPrintf(BASECON_TYPE_NORMAL,
        "%s:%d  %s\n",
        File ? File : "?", Line, Func ? Func : "?");

    // Регистры
    if (Frame) {
        PanicDumpRegsFromFrame(Frame);
    } else {
        BaseconPrintf(BASECON_TYPE_NORMAL, "[REG]\n");
        PanicReg4("RIP", FallbackRip, "RBP", GetRbp(),
                  "---", 0, "---", 0);
    }

    // Control regs
    PanicDumpControlRegs();

    // Stack
    UINTPTR Rip = Frame ? Frame->RIP : FallbackRip;
    UINTPTR Rbp = Frame ? Frame->RBP : GetRbp();
    PanicDumpStack(Rip, Rbp);

    // Финал
    BaseconSetColors(RGB_RED, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "HALTED\n");
    BaseconSetColors(RGB_GRAY, RGB_BLACK);
}

// ============================================================================
// PUBLIC API
// ============================================================================

VOID FatalWithFrame(const CHAR *Message, InterruptFrame *Frame,
                    const CHAR *File, INT Line, const CHAR *Func) {
    if (PanicAlreadyRunning) return;
    PanicAlreadyRunning = TRUE;

    PanicShow(Message, File, Line, Func, Frame, 0);

    for (;;) Halt();
}

VOID FatalImpl(const CHAR *Message, const CHAR *File, INT Line,
               const CHAR *Func, UINTPTR CallerRip) {
    if (PanicAlreadyRunning) return;
    PanicAlreadyRunning = TRUE;

    PanicShow(Message, File, Line, Func, NULLPTR, CallerRip);

    for (;;) Halt();
}