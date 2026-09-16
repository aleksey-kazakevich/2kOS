#include <Lib/String.h>
#include <Cmd/Touch.h>
#include <Basecon.h>
#include <Fs/Vfs.h>
#include <Return.h>

INT CmdTouchHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Usage: touch <file>\n");
        return -1;
    }

    VfsInode *File;
    INT Result = VfsCreate(VfsCurrentDir, ArgV[1], FT_REG_FILE, &File);
    
    if (Result != 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Failed to create file: %s: %s\n", ArgV[1], ReturnCode2String(Result));
        return -1;
    }

    VfsInodeUnref(File);
    BaseconPrintf(BASECON_TYPE_SUCCESS, "File created: %s\n", ArgV[1]);
    return 0;
}

INT CmdTouchRegister(VOID) {
    return CmdRegisterBuiltin("touch", "Create an empty file", CmdTouchHandler);
}