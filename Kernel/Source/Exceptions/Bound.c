#include <Utils/ExceptionLog.h>
#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID BoundHandler(InterruptFrame *Frame) {
    if (ExceptionFromUser(Frame)) {
        ExceptionLogPush(EXC_BOUND_RANGE, Frame);
    }
    HANDLE_EXCEPTION(Frame, "bound range exceeded");
    for (;;) Halt();
}
