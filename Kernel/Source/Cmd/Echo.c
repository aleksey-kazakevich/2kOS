#include <Lib/String.h>
#include <Cmd/Echo.h>
#include <Basecon.h>
#include <Return.h>

INT CmdEchoHandler(INT ArgC, const CHAR *ArgV[]) {
    // If there are no arguments, just output an empty string.
    if (ArgC < 2) {
        BaseconPutString("\n");
        return 0;
    }
    
    // Output all arguments separated by a space
    for (INT I = 1; I < ArgC; I++) {
        BaseconPutString(ArgV[I]);
        if (I < ArgC - 1) {
            BaseconPutString(" ");
        }
    }
    BaseconPutString("\n");
    
    return 0;
}

INT CmdEchoRegister(VOID) {
    return CmdRegisterBuiltin("echo", "Echo text to console", CmdEchoHandler);
}