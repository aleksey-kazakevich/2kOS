#include <Types.h>
#include <ExceptionHelper.h>
#include <Asm/Cpu.h>
#include <Interrupt.h>

VOID GeneralProtectionHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "general protection");
}
