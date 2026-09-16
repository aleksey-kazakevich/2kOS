#include <Lib/String.h>
#include <Cmd/Rmdir.h>
#include <Basecon.h>
#include <Fs/Vfs.h>
#include <Return.h>

INT CmdRmdirHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Usage: rmdir <path>\n");
        return -1;
    }

    INT Result = VfsRmdir(VfsCurrentDir, ArgV[1]);
    
    if (Result != 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Failed to remove directory: %s\n", ArgV[1]);
        BaseconPrintf(BASECON_TYPE_ERROR, "Directory may be non-empty or not exist\n");
        return -1;
    }

    BaseconPrintf(BASECON_TYPE_SUCCESS, "Directory removed: %s\n", ArgV[1]);
    return 0;
}

INT CmdRmdirRegister(VOID) {
    return CmdRegisterBuiltin("rmdir", "Remove an empty directory", CmdRmdirHandler);
}