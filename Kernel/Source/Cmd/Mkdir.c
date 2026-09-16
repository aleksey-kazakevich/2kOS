#include <Lib/String.h>
#include <Cmd/Mkdir.h>
#include <Basecon.h>
#include <Fs/Vfs.h>
#include <Return.h>

INT CmdMkdirHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Usage: mkdir <path>\n");
        return -1;
    }

    VfsInode *NewDir;
    INT Result = VfsMkdir(VfsCurrentDir, ArgV[1], FT_DIR, &NewDir);
    
    if (Result != 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Failed to create directory: %s\n", ArgV[1]);
        return -1;
    }

    VfsInodeUnref(NewDir);
    BaseconPrintf(BASECON_TYPE_SUCCESS, "Directory created: %s\n", ArgV[1]);
    return 0;
}

INT CmdMkdirRegister(VOID) {
    return CmdRegisterBuiltin("mkdir", "Create a directory", CmdMkdirHandler);
}