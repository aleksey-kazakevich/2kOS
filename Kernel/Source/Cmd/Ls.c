#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Basecon.h>
#include <Cmd/Ls.h>
#include <Fs/Vfs.h>
#include <Return.h>
#include <Rgb.h>

// Colors
#define LS_DIR   RGB_CORNFLOWER_BLUE
#define LS_FILE  RGB_GRAY
#define LS_ERR   RGB_RED

STATIC VOID CmdLsPrintHelp(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "Usage: ls [path]\n");
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
            BaseconPrintf(BASECON_TYPE_ERROR, "Unknown option: %s\n", ArgV[I]);
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
            BaseconPrintf(BASECON_TYPE_ERROR, "ls: cannot access '%s'\n", Path);
            return -1;
        }
        if (Dir->IMode != FT_DIR) {
            VfsInodeUnref(Dir);
            BaseconPrintf(BASECON_TYPE_ERROR, "ls: '%s' is not a directory\n", Path);
            return -1;
        }
    }

    // We run through the content and output it in one line separated by a space.
    FramebufferColor PrevColor = RGB_GRAY;
    BaseconSetColors(LS_FILE, RGB_BLACK);

    while (VfsReadDir(Dir, &Pos, Name, &NameLen, &Type) == 0) {
        // Skip . and ..
        if (StrCmp(Name, ".") == 0 || StrCmp(Name, "..") == 0)
            continue;

        if (Count > 0)
            BaseconPrintf(BASECON_TYPE_NORMAL, "  ");  // 2 spaces between names

        if (Type == FT_DIR) {
            BaseconSetColors(LS_DIR, RGB_BLACK);
            BaseconPrintf(BASECON_TYPE_NORMAL, "%s/", Name);
            BaseconSetColors(LS_FILE, RGB_BLACK);
        } else {
            BaseconPrintf(BASECON_TYPE_NORMAL, "%s", Name);
        }

        Count++;
    }

    if (Count > 0)
        BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    else
        BaseconPrintf(BASECON_TYPE_NORMAL, "\n");

    (VOID)PrevColor;
    VfsInodeUnref(Dir);
    return 0;
}

INT CmdLsRegister(VOID) {
    return CmdRegisterBuiltin("ls", "List directory contents", CmdLsHandler);
}