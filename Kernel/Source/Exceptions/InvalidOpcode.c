#include <Types.h>
#include <Asm/Cpu.h>
#include <ExceptionHelper.h>
#include <Interrupt.h>

/* Main #UD handler*/
VOID InvalidOpcodeHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "invalid opcode");
}
