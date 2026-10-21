#include <Utils/ExceptionLog.h>
#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

/* Main #UD handler */
VOID InvalidOpcodeHandler(InterruptFrame *Frame) {
    if (ExceptionFromUser(Frame)) {
        ExceptionLogPush(EXC_INVALID_OPCODE, Frame);
    }
    HANDLE_EXCEPTION(Frame, "invalid opcode");
}
