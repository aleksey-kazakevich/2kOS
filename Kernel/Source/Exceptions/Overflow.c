#include <Utils/ExceptionLog.h>
#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID OverflowHandler(InterruptFrame *Frame) {
    if (ExceptionFromUser(Frame)) {
        ExceptionLogPush(EXC_OVERFLOW, Frame);
    }
    HANDLE_EXCEPTION(Frame, "overflow");
}
