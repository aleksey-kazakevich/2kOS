#include <Types.h>
#include <Fatal.h>
#include <Asm/Cpu.h>
#include <Interrupt.h>
#include <Fatal.h>
#include <ExceptionHelper.h>
#include <Basecon.h>

// Mask for clearing DR6 status bits (Intel SDM Vol 3, 17.4.2)
// Bits 0-3, 13-15 are reset to 0. Reserved bits remain 1.
#define DR6_CLEAR_MASK  0xFFFF0FF0

static inline UINT64 ReadDr6(VOID) {
    UINT64 Val;
    asm volatile("mov %%dr6, %0" : "=r"(Val));
    return Val;
}

static inline VOID WriteDr6(UINT64 Val) {
    asm volatile("mov %0, %%dr6" : : "r"(Val));
}

VOID DebugExceptionHandler(InterruptFrame *Frame) {
    BOOL FromUser = ExceptionFromUser(Frame);
    
    if (!FromUser) {
        UINT64 Dr6 = ReadDr6();
        
        WriteDr6(DR6_CLEAR_MASK);
        
        FatalF("kernel debug exception", Frame);
    } else {
        WriteDr6(DR6_CLEAR_MASK);
    }
    
    return;
}

