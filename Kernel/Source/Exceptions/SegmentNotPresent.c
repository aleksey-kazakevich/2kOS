#include <Utils/ExceptionLog.h>
#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID SegmentNotPresentHandler(InterruptFrame *Frame) {
    if (ExceptionFromUser(Frame)) {
        ExceptionLogPush(EXC_SEGMENT_NOT_PRESENT, Frame);
    }
    HANDLE_EXCEPTION(Frame, "segment not present");
}
