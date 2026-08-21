#include <Types.h>
#include <ExceptionHelper.h>
#include <Asm/Cpu.h>
#include <Interrupt.h>

VOID SegmentNotPresentHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "segment not present");
}
