#include <Types.h>
#include <ExceptionHelper.h>
#include <Asm/Cpu.h>
#include <Interrupt.h>

VOID FpuErrorHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "fpu error");
}
