#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <Cmd/Ls.h>
#include <Fs/Vfs.h>
#include <Return.h>
#include <Rgb.h>

STATIC VOID CmdLsPrintHelp(VOID) {
    StdOutPrintf("Usage: ls [path]\n");
}

INT CmdLsHandler(INT ArgC, const CHAR *ArgV[]) {
    VfsInode *Dir;
    UINT64 Pos = 0;
    CHAR Name[256];
    UINT32 NameLen;
    UINT32 Type;
    INT Count = 0;
    const CHAR *Path = NULLPTR;

    // Parsim argument (path only, no options — for now)
    for (INT I = 1; I < ArgC; I++) {
        if (ArgV[I][0] == '-') {
            StdErrPrintf("Unknown option: %s\n", ArgV[I]);
            CmdLsPrintHelp();
            return -1;
        }
        Path = ArgV[I];
    }

    // Opening the directory
    if (!Path) {
        Dir = VfsCurrentDir;
        VfsInodeRef(Dir);
    } else {
        if (VfsWalk(VfsCurrentDir, Path, &Dir) != 0) {
            StdErrPrintf("ls: cannot access '%s'\n", Path);
            return -1;
        }
        if (Dir->IMode != FT_DIR) {
            VfsInodeUnref(Dir);
            StdErrPrintf("ls: '%s' is not a directory\n", Path);
            return -1;
        }
    }

    // We run through the content and output it in one line separated by a space.
    while (VfsReadDir(Dir, &Pos, Name, &NameLen, &Type) == 0) {
        // Skip . and ..
        if (StrCmp(Name, ".") == 0 || StrCmp(Name, "..") == 0)
            continue;

        if (Count > 0)
            StdOutPrintf("  ");  // 2 spaces between names

        if (Type == FT_DIR) {
            StdOutPrintf("%s/", Name);
        } else {
            StdOutPrintf("%s", Name);
        }

        Count++;
    }

    if (Count > 0)
        StdOutPrintf("\n");
    else
        StdOutPrintf("\n");

    VfsInodeUnref(Dir);
    return 0;
}

INT CmdLsRegister(VOID) {
    return CmdRegisterBuiltin("ls", "List directory contents", CmdLsHandler);
}