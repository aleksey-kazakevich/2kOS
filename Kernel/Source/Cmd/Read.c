#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Cmd/Read.h>
#include <Basecon.h>
#include <Fs/Vfs.h>
#include <Return.h>

#define READ_CHUNK_SIZE 512

INT CmdReadHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Usage: read <file>\n");
        return -1;
    }

    if (ArgC > 2) {
        BaseconPrintf(BASECON_TYPE_ERROR, "read: too many arguments\n");
        return -1;
    }

    const CHAR *Path = ArgV[1];

    // 1. Opening the file for reading
    VfsFile *File = NULLPTR;
    INT Rc = VfsOpen(VfsCurrentDir, Path, O_READ, &File);
    if (Rc != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "read: cannot open '%s': %s\n",
                      Path, ReturnCode2String(Rc));
        return -1;
    }

    // 2. We check that this is a regular file.
    if (File->FInode->IMode != FT_REG_FILE) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "read: '%s' is not a regular file\n", Path);
        VfsClose(File);
        return -1;
    }

    // 3. Special case: an empty file
    if (File->FInode->ISize == 0) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
        VfsClose(File);
        return 0;
    }

    // 4. We read in portions and output
    CHAR Buf[READ_CHUNK_SIZE + 1];
    UINT32 TotalRead = 0;

    while (1) {
        UINT32 Got = 0;
        Rc = VfsRead(File, Buf, READ_CHUNK_SIZE, &Got);

        if (Rc != SUCCESS) {
            BaseconPrintf(BASECON_TYPE_ERROR,
                          "\nread: I/O error at offset %u\n", TotalRead);
            VfsClose(File);
            return -1;
        }

        if (Got == 0) {
            // End of file
            break;
        }

        Buf[Got] = '\0';
        BaseconPutString(Buf);

        TotalRead += Got;

        if (Got < READ_CHUNK_SIZE) {
            // The last piece
            break;
        }
    }

    // 5. If the file did not end with a line break, add it.,
    //    so that the prompt doesn't stick to the last line
    if (TotalRead > 0 && Buf[TotalRead % READ_CHUNK_SIZE ? 
                            TotalRead % READ_CHUNK_SIZE - 1 : 
                            READ_CHUNK_SIZE - 1] != '\n') {
        BaseconPutString("\n");
    }

    VfsClose(File);
    return 0;
}

INT CmdReadRegister(VOID) {
    return CmdRegisterBuiltin("read",
                              "Print file contents: read <file>",
                              CmdReadHandler);
}