#include <Types.h>
#include <ExceptionHelper.h>
#include <Asm/Cpu.h>
#include <Interrupt.h>

VOID StackSegmentHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "stack segment fault");
}
