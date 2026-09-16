#include <Lib/String.h>
#include <Basecon.h>
#include <Cmd/Rm.h>
#include <Fs/Vfs.h>
#include <Return.h>

INT CmdRmHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Usage: rm <file>\n");
        return -1;
    }

    INT Result = VfsUnlink(VfsCurrentDir, ArgV[1]);
    
    if (Result != 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Failed to remove: %s\n", ArgV[1]);
        BaseconPrintf(BASECON_TYPE_ERROR, "File may not exist or is a directory\n");
        return -1;
    }

    BaseconPrintf(BASECON_TYPE_SUCCESS, "Removed: %s\n", ArgV[1]);
    return 0;
}

INT CmdRmRegister(VOID) {
    return CmdRegisterBuiltin("rm", "Remove a file", CmdRmHandler);
}