#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID DivideErrorHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "divide error");
}
