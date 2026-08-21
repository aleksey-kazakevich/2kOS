#include <Types.h>
#include <Asm/Cpu.h>
#include <ExceptionHelper.h>
#include <Interrupt.h>

VOID DivideErrorHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "divide error");
}
