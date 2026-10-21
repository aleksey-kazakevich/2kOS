#include <Cmd.h>
#include <Cmd/Asm.h>
#include <Utils/Asm.h>
#include <Basecon.h>

INT CmdAsmHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Usage: asm <input.asm> [output.elf]\n");
        return -1;
    }
    const CHAR *In = ArgV[1];
    const CHAR *Out = (ArgC >= 3) ? ArgV[2] : NULLPTR;

    AsmResult R = AsmCompileFile(In, Out);
    if (R.Code != ASM_OK) {
        BaseconPrintf(BASECON_TYPE_ERROR, "asm: line %u: %s\n", R.Line, R.Message);
        return -1;
    }
    BaseconPrintf(BASECON_TYPE_SUCCESS, "asm: %s\n", R.Message);
    return 0;
}

INT CmdAsmRegister(VOID) {
    return CmdRegisterBuiltin("asm", "Assemble .asm to ELF64", CmdAsmHandler);
}