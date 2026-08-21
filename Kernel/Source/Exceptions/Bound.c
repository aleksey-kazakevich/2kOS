#include <Types.h>
#include <ExceptionHelper.h>
#include <Asm/Cpu.h>
#include <Interrupt.h>

VOID BoundHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "bound range exceeded");
    for (;;) Halt();
}
