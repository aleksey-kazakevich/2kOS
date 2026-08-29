#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID InvalidTssHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "invalid tss");
    
    for (;;) Halt();
}
