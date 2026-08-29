#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID BoundHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "bound range exceeded");
    for (;;) Halt();
}
