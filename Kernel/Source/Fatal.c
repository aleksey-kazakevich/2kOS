#include <Fatal.h>
#include <Basecon.h>
#include <Asm/Cpu.h>
#include <Rgb.h>

#define MAX_FRAMES 32

static BOOL PanicAlreadyRunning = FALSE;

typedef struct {
    UINTPTR Rip;
    UINTPTR Rbp;
} StackFrame;

USIZE UnwindStack(UINTPTR *Buffer, USIZE MaxFrames, UINTPTR CallerRip) {
    USIZE Count = 0;
    
    // We get the current RBP and RIP
    UINTPTR Rbp = GetRbp();
    UINTPTR Rip = CallerRip;
    
    Buffer[Count++] = Rip;
    
    // We go through the RBP chain
    while (Count < MaxFrames && Rbp && Rbp > 0x1000) {
        UINTPTR NextRip = *(volatile UINTPTR*)(Rbp + 8);
        UINTPTR NextRbp = *(UINTPTR*)(Rbp);
        
        if (NextRip == 0) break;  // End of the chain
        
        Buffer[Count++] = NextRip;
        Rbp = NextRbp;
    }
    
    return Count;
}

VOID PrintStacktrace(UINTPTR CallerRip) {
    UINTPTR Frames[MAX_FRAMES];
    USIZE Count = UnwindStack(Frames, MAX_FRAMES, CallerRip);
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "    stack trace:\n");
    for (USIZE I = 0; I < Count; I++) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "        %p\n", (VOID*)(Frames[I]));
    }
}

static USIZE UnwindStackFromRip(UINTPTR *Buffer, USIZE MaxFrames, UINTPTR Rip, UINTPTR Rbp) {
    USIZE Count = 0;
    
    // Add the RIP that caused the exception
    Buffer[Count++] = Rip;
    
    // We follow the RBP chain from the frame
    while (Count < MaxFrames && Rbp && Rbp > 0x1000) {
        UINTPTR NextRip = *(volatile UINTPTR*)(Rbp + 8);
        UINTPTR NextRbp = *(UINTPTR*)(Rbp);
        
        if (NextRip == 0) break;
        
        Buffer[Count++] = NextRip;
        Rbp = NextRbp;
    }
    
    return Count;
}

VOID FatalWithFrame(const CHAR *Message, InterruptFrame *Frame, const CHAR *File, INT Line, const CHAR *Func) {
    if (PanicAlreadyRunning == TRUE) return;

    PanicAlreadyRunning = TRUE;
    LocalInterruptsDisable();
    BaseconClear();
    BaseconSetColors(RGB_DARK_RED, RGB_BLACK);
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "fatal:\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "    stop: %s\n", Message ? Message : "(null)");
    BaseconPrintf(BASECON_TYPE_NORMAL, "    file: %s\n", File ? File : "(null)");
    BaseconPrintf(BASECON_TYPE_NORMAL, "    line: %d\n", Line);
    BaseconPrintf(BASECON_TYPE_NORMAL, "    func: %s\n", Func ? Func : "?");
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "    exception rip: 0x%016X\n", Frame->RIP);
    BaseconPrintf(BASECON_TYPE_NORMAL, "    cs: 0x%X\n", Frame->CS);
    BaseconPrintf(BASECON_TYPE_NORMAL, "    rflags: 0x%X\n", Frame->RFLAGS);
    BaseconPrintf(BASECON_TYPE_NORMAL, "    rsp: 0x%X\n", Frame->RSP);
    
    if (Frame->ErrorCode != 0xFFFFFFFF)
        BaseconPrintf(BASECON_TYPE_NORMAL, "    error code: 0x%X\n", Frame->ErrorCode);
    
    // Call stack from CORRECT RIP
    BaseconPrintf(BASECON_TYPE_NORMAL, "    stack trace:\n");
    UINTPTR Frames[MAX_FRAMES];
    USIZE Count = UnwindStackFromRip(Frames, MAX_FRAMES, Frame->RIP, Frame->RBP);
    for (USIZE I = 0; I < Count; I++) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "        %p\n", (VOID*)(Frames[I]));
    }
    
    Halt();
}

VOID FatalImpl(const CHAR *Message, const CHAR *File, INT Line, const CHAR *Func, UINTPTR CallerRip) {
    if (PanicAlreadyRunning == TRUE) return;

    PanicAlreadyRunning = TRUE;

    LocalInterruptsDisable();

    BaseconClear();

    BaseconSetColors(RGB_DARK_RED, RGB_BLACK);
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n\n");
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "fatal:\n");
    const CHAR *MsgSafe = Message ? Message : "(null)";
    const CHAR *FileSafe = File ? File : "(null)";
    BaseconPrintf(BASECON_TYPE_NORMAL, "    stop: %s\n", MsgSafe);
    BaseconPrintf(BASECON_TYPE_NORMAL, "    file: %s\n", FileSafe);
    BaseconPrintf(BASECON_TYPE_NORMAL, "    line: %d\n", Line);
    const CHAR *FuncName = Func ? Func : "?";
    BaseconPrintf(BASECON_TYPE_NORMAL, "    func: %s (%p)\n", FuncName, (VOID*)CallerRip);
    
    PrintStacktrace(CallerRip);
    
    Halt();
}
