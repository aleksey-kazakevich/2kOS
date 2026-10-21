#include <Lib/String.h>
#include <Cmd/Cd.h>
#include <Basecon.h>
#include <Fs/Vfs.h>
#include <Return.h>

INT CmdCdHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        // Without an argument, go to the root of the current disk.
        if (VfsCd(&VfsCurrentDir, "/") != 0) {
            BaseconPrintf(BASECON_TYPE_ERROR, "cd: cannot go to root\n");
            return -1;
        }
        return 0;
    }

    if (ArgC > 2) {
        BaseconPrintf(BASECON_TYPE_ERROR, "cd: too many arguments\n");
        return -1;
    }

    if (VfsCd(&VfsCurrentDir, ArgV[1]) != 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "cd: no such directory: %s\n", ArgV[1]);
        return -1;
    }

    return 0;
}

INT CmdCdRegister(VOID) {
    return CmdRegisterBuiltin("cd", "Change current directory", CmdCdHandler);
}