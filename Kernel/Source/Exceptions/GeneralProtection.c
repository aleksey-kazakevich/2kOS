#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID GeneralProtectionHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "general protection");
}
