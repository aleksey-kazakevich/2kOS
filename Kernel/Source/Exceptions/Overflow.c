#include <Types.h>
#include <ExceptionHelper.h>
#include <Asm/Cpu.h>
#include <Interrupt.h>

VOID OverflowHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "overflow");
}
