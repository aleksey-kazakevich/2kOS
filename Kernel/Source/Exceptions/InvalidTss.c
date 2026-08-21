#include <Types.h>
#include <ExceptionHelper.h>
#include <Asm/Cpu.h>
#include <Interrupt.h>

VOID InvalidTssHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "invalid tss");
    
    for (;;) Halt();
}
