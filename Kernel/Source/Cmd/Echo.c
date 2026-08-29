#include <Lib/String.h>
#include <Cmd/Echo.h>
#include <Basecon.h>
#include <Return.h>

INT CmdEchoHandler(INT ArgC, const CHAR *ArgV[]) {
    // Если нет аргументов, просто выводим пустую строку
    if (ArgC < 2) {
        BaseconPutChar('\n');
        return 0;
    }
    
    // Выводим все аргументы через пробел
    for (INT I = 1; I < ArgC; I++) {
        BaseconPutString(ArgV[I]);
        if (I < ArgC - 1) {
            BaseconPutChar(' ');
        }
    }
    BaseconPutChar('\n');
    
    return 0;
}

INT CmdEchoRegister(VOID) {
    return CmdRegisterBuiltin("echo", "Echo text to console", CmdEchoHandler);
}