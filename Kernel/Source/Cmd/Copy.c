#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Cmd/Copy.h>
#include <Basecon.h>
#include <Fs/Vfs.h>
#include <Return.h>

#define COPY_CHUNK_SIZE (64 * 1024)   /* 64 KiB per read/write */

STATIC VOID CmdCopyHelp(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Usage: copy <source> <dest>\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  source  - existing file path\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  dest    - destination path\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Examples:\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  copy a.txt b.txt              (same directory)\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  copy SATA0::/a.txt SATA1::/b.txt\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
}

/* Convert a VFS return code into a printable string. */
STATIC const CHAR *VfsErr(INT Rc) {
    const CHAR *S = ReturnCode2String(Rc);
    return S ? S : "UNKNOWN";
}

INT CmdCopyHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 3) {
        CmdCopyHelp();
        return -1;
    }
    if (ArgC > 3) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "copy: too many arguments\n");
        CmdCopyHelp();
        return -1;
    }

    const CHAR *SrcPath = ArgV[1];
    const CHAR *DstPath = ArgV[2];

    /* --- 1. Open the source --- */
    VfsFile *Src = NULLPTR;
    INT Rc = VfsOpen(VfsCurrentDir, SrcPath, O_READ, &Src);
    if (Rc != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "copy: cannot open '%s': %s\n",
                      SrcPath, VfsErr(Rc));
        return -1;
    }

    if (Src->FInode->IMode == FT_DIR) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "copy: '%s' is a directory (recursive copy not supported)\n",
                      SrcPath);
        VfsClose(Src);
        return -1;
    }
    if (Src->FInode->IMode != FT_REG_FILE) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "copy: '%s' is not a regular file\n", SrcPath);
        VfsClose(Src);
        return -1;
    }

    /* --- 2. Resolve the destination --- */
    BOOL DestIsDir = FALSE;

    /* 2a. If dest already exists and is a directory, append source basename. */
    {
        VfsInode *Existing = NULLPTR;
        if (VfsWalk(VfsCurrentDir, DstPath, &Existing) == SUCCESS) {
            if (Existing->IMode == FT_DIR) {
                DestIsDir = TRUE;
            }
            VfsInodeUnref(Existing);
        }
    }

    CHAR FinalDst[PATH_MAX];
    if (DestIsDir) {
        /* Extract basename from the source path. */
        const CHAR *Base = SrcPath;
        const CHAR *P = SrcPath;
        while (*P) {
            if (*P == '/') Base = P + 1;
            P++;
        }
        if (*Base == '\0') {
            BaseconPrintf(BASECON_TYPE_ERROR,
                          "copy: cannot derive filename from '%s'\n", SrcPath);
            VfsClose(Src);
            return -1;
        }

        /* Build the final path: "<dst>/<basename>".
         * Normalize so we don't end up with a double slash. */
        USIZE DstLen = StrLen(DstPath);
        if (DstLen > 0 && DstPath[DstLen - 1] == '/') {
            SnPrintf(FinalDst, sizeof(FinalDst), "%s%s", DstPath, Base);
        } else {
            SnPrintf(FinalDst, sizeof(FinalDst), "%s/%s", DstPath, Base);
        }
    } else {
        StrnCpy(FinalDst, DstPath, sizeof(FinalDst) - 1);
        FinalDst[sizeof(FinalDst) - 1] = '\0';
    }

    /* --- 3. Refuse to copy a file onto itself --- */
    {
        VfsInode *Tmp = NULLPTR;
        if (VfsWalk(VfsCurrentDir, FinalDst, &Tmp) == SUCCESS) {
            if (Tmp == Src->FInode) {
                BaseconPrintf(BASECON_TYPE_ERROR,
                              "copy: source and destination are the same file\n");
                VfsInodeUnref(Tmp);
                VfsClose(Src);
                return -1;
            }
            VfsInodeUnref(Tmp);
        }
    }

    /* --- 4. Open the destination (create / truncate) --- */
    VfsFile *Dst = NULLPTR;
    Rc = VfsOpen(VfsCurrentDir, FinalDst,
                 O_WRITE | O_CREAT | O_TRUNC, &Dst);
    if (Rc != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "copy: cannot create '%s': %s\n",
                      FinalDst, VfsErr(Rc));
        VfsClose(Src);
        return -1;
    }

    /* --- 5. Copy the data --- */
    UINT8 *Buf = (UINT8 *)MemoryAllocate(COPY_CHUNK_SIZE);
    if (!Buf) {
        BaseconPrintf(BASECON_TYPE_ERROR, "copy: out of memory\n");
        VfsClose(Dst);
        VfsClose(Src);
        return -1;
    }

    UINT64 Total = 0;
    INT FailRc = SUCCESS;

    for (;;) {
        UINT32 Got = 0;
        Rc = VfsRead(Src, Buf, COPY_CHUNK_SIZE, &Got);
        if (Rc != SUCCESS) {
            FailRc = Rc;
            break;
        }
        if (Got == 0) break;   /* EOF */

        /* Handle partial writes: loop until the whole chunk is written. */
        UINT32 Done = 0;
        while (Done < Got) {
            UINT32 Wr = 0;
            Rc = VfsWrite(Dst, Buf + Done, Got - Done, &Wr);
            if (Rc != SUCCESS || Wr == 0) {
                FailRc = (Rc != SUCCESS) ? Rc : IO_ERROR;
                break;
            }
            Done += Wr;
        }
        if (FailRc != SUCCESS) break;

        Total += Got;
    }

    MemoryFree(Buf);

    /* --- 6. Close (VfsClose syncs dirty inodes) --- */
    VfsClose(Dst);
    VfsClose(Src);

    if (FailRc != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "copy: failed after %llu bytes: %s\n",
                      Total, VfsErr(FailRc));
        return -1;
    }

    BaseconPrintf(BASECON_TYPE_SUCCESS,
                  "copy: '%s' -> '%s' (%llu bytes)\n",
                  SrcPath, FinalDst, Total);
    return 0;
}

INT CmdCopyRegister(VOID) {
    return CmdRegisterBuiltin("copy",
                              "Copy a file: copy <src> <dst>",
                              CmdCopyHandler);
}