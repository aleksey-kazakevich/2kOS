#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID StackSegmentHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "stack segment fault");
}
