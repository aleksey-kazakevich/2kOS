#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

/* Main #UD handler */
VOID InvalidOpcodeHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "invalid opcode");
}
