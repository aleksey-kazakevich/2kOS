#include <Utils/ExceptionLog.h>
#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID GeneralProtectionHandler(InterruptFrame *Frame) {
    if (ExceptionFromUser(Frame)) {
        ExceptionLogPush(EXC_GENERAL_PROTECTION, Frame);
    }
    HANDLE_EXCEPTION(Frame, "general protection");
}
