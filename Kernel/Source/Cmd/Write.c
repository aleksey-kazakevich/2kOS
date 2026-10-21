#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Cmd/Write.h>
#include <Basecon.h>
#include <Fs/Vfs.h>
#include <Return.h>

INT CmdWriteHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 4) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Usage: write <text> > <file>\n");
        return -1;
    }

    // 1. Looking for '>'
    INT RedirIdx = -1;
    for (INT I = 1; I < ArgC; I++) {
        if (StrCmp(ArgV[I], ">") == 0) {
            RedirIdx = I;
            break;
        }
    }

    if (RedirIdx < 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "write: missing '>' redirection\n");
        BaseconPrintf(BASECON_TYPE_NORMAL, "Usage: write <text> > <file>\n");
        return -1;
    }

    if (RedirIdx == 1) {
        BaseconPrintf(BASECON_TYPE_ERROR, "write: empty text\n");
        return -1;
    }

    if (RedirIdx + 1 >= ArgC) {
        BaseconPrintf(BASECON_TYPE_ERROR, "write: missing file name\n");
        return -1;
    }

    const CHAR *FilePath = ArgV[RedirIdx + 1];

    // 2. Gluing the text from ArgV[1..Redirecdx-1] separated by a space
    USIZE TextLen = 0;
    for (INT I = 1; I < RedirIdx; I++) {
        TextLen += StrLen(ArgV[I]);
        if (I < RedirIdx - 1) TextLen++; // whitespace
    }

    CHAR *Text = (CHAR *)MemoryAllocate(TextLen + 1);
    if (!Text) {
        BaseconPrintf(BASECON_TYPE_ERROR, "write: out of memory\n");
        return -1;
    }

    Text[0] = '\0';
    for (INT I = 1; I < RedirIdx; I++) {
        StrCat(Text, ArgV[I]);
        if (I < RedirIdx - 1) StrCat(Text, " ");
    }

    // 3. Open the file (create it if not)
    VfsFile *File = NULLPTR;
    INT Rc = VfsOpen(VfsCurrentDir, FilePath,
                     O_WRITE | O_CREAT | O_APPEND, &File);
    if (Rc != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "write: cannot open '%s': %s\n",
                      FilePath, ReturnCode2String(Rc));
        MemoryFree(Text);
        return -1;
    }

    // 4. If the file is not empty, first write '\n'
    UINT32 Written = 0;
    if (File->FInode->ISize > 0) {
        Rc = VfsWrite(File, "\n", 1, &Written);
        if (Rc != SUCCESS || Written != 1) {
            BaseconPrintf(BASECON_TYPE_ERROR, "write: cannot write newline\n");
            VfsClose(File);
            MemoryFree(Text);
            return -1;
        }
    }

    // 5. We write the text itself
    Rc = VfsWrite(File, Text, (UINT32)TextLen, &Written);
    if (Rc != SUCCESS || Written != (UINT32)TextLen) {
        BaseconPrintf(BASECON_TYPE_ERROR, "write: cannot write text\n");
        VfsClose(File);
        MemoryFree(Text);
        return -1;
    }

    VfsClose(File);
    MemoryFree(Text);

    return 0;
}

INT CmdWriteRegister(VOID) {
    return CmdRegisterBuiltin("write",
                              "Write text to file: write <text> > <file>",
                              CmdWriteHandler);
}