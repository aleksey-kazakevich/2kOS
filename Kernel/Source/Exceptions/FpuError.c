#include <Utils/ExceptionLog.h>
#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID FpuErrorHandler(InterruptFrame *Frame) {
    if (ExceptionFromUser(Frame)) {
        ExceptionLogPush(EXC_FPU_ERROR, Frame);
    }
    HANDLE_EXCEPTION(Frame, "fpu error");
}
