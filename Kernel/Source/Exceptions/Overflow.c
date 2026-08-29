#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID OverflowHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "overflow");
}
