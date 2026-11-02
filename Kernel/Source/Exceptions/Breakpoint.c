#include <Utils/ExceptionLog.h>
#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Types.h>

VOID BreakpointHandler(InterruptFrame *Frame) {
    if (ExceptionFromUser(Frame)) {
        ExceptionLogPush(EXC_BREAKPOINT, Frame);
    }

    Frame->RIP += 1;
    
    return;
}
