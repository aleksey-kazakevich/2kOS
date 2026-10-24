#include <Utils/Asm.h>
#include <Io/StdErr.h>
#include <Io/StdOut.h>
#include <Cmd/Asm.h>
#include <Cmd.h>

INT CmdAsmHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        StdErrPrintf("Usage: asm <input.asm> [output.elf]\n");
        return -1;
    }
    const CHAR *In = ArgV[1];
    const CHAR *Out = (ArgC >= 3) ? ArgV[2] : NULLPTR;

    AsmResult R = AsmCompileFile(In, Out);
    if (R.Code != ASM_OK) {
        StdErrPrintf("asm: line %u: %s\n", R.Line, R.Message);
        return -1;
    }
    StdOutPrintf("asm: %s\n", R.Message);
    return 0;
}

INT CmdAsmRegister(VOID) {
    return CmdRegisterBuiltin("asm", "Assemble .asm to ELF64", CmdAsmHandler);
}