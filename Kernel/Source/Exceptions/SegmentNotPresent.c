#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID SegmentNotPresentHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "segment not present");
}
