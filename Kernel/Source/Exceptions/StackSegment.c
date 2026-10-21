#include <Utils/ExceptionLog.h>
#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID StackSegmentHandler(InterruptFrame *Frame) {
    if (ExceptionFromUser(Frame)) {
        ExceptionLogPush(EXC_STACK_SEGMENT_FAULT, Frame);
    }
    HANDLE_EXCEPTION(Frame, "stack segment fault");
}
