#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID FpuErrorHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "fpu error");
}
