#include <Utils/ExceptionLog.h>
#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID InvalidTssHandler(InterruptFrame *Frame) {
    if (ExceptionFromUser(Frame)) {
        ExceptionLogPush(EXC_INVALID_TSS, Frame);
    }
    HANDLE_EXCEPTION(Frame, "invalid tss");
    for (;;) Halt();
}
