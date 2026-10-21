#include <Mem/Allocator.h>
#include <Cmd/HexDump.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Basecon.h>
#include <Fs/Vfs.h>
#include <Return.h>
#include <Rgb.h>

#define HEXDUMP_LINE_BYTES 16

STATIC VOID HexDumpHelp(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Usage: hexdump <file> [offset] [length]\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  offset  - decimal or 0xHEX (default: 0)\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  length  - decimal or 0xHEX (default: 256)\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
}

STATIC UINT64 ParseNum(const CHAR *S) {
    if (!S) return 0;
    if (S[0] == '0' && (S[1] == 'x' || S[1] == 'X')) {
        UINT64 V = 0;
        S += 2;
        while (*S) {
            CHAR C = *S++;
            UINT8 D;
            if (C >= '0' && C <= '9') D = C - '0';
            else if (C >= 'a' && C <= 'f') D = C - 'a' + 10;
            else if (C >= 'A' && C <= 'F') D = C - 'A' + 10;
            else break;
            V = (V << 4) | D;
        }
        return V;
    }
    return (UINT64)AToI(S);
}

// Prints one byte as 2 hex digits
STATIC VOID PutHex8(UINT8 B) {
    const CHAR *Hex = "0123456789ABCDEF";
    CHAR Buf[3];
    Buf[0] = Hex[(B >> 4) & 0xF];
    Buf[1] = Hex[B & 0xF];
    Buf[2] = '\0';
    BaseconPutString(Buf);
}

// Prints ASCII (printable characters, the rest are dots)
STATIC VOID PutAscii(UINT8 B) {
    CHAR Buf[2];
    if (B >= 0x20 && B <= 0x7E) Buf[0] = (CHAR)B;
    else Buf[0] = '.';
    Buf[1] = '\0';
    BaseconPutString(Buf);
}

INT CmdHexDumpHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        HexDumpHelp();
        return -1;
    }

    const CHAR *Path = ArgV[1];
    UINT64 Offset = (ArgC >= 3) ? ParseNum(ArgV[2]) : 0;
    UINT64 Length = (ArgC >= 4) ? ParseNum(ArgV[3]) : 256;

    if (Length == 0 || Length > 65536) Length = 65536;

    // Opening the file
    VfsFile *File = NULLPTR;
    INT Rc = VfsOpen(VfsCurrentDir, Path, O_READ, &File);
    if (Rc != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR, "hexdump: cannot open '%s'\n", Path);
        return -1;
    }

    UINT64 FileSize = File->FInode ? File->FInode->ISize : 0;
    if (Offset >= FileSize) {
        BaseconPrintf(BASECON_TYPE_WARNING,
                      "hexdump: offset 0x%llX >= file size 0x%llX\n",
                      Offset, FileSize);
        VfsClose(File);
        return -1;
    }

    if (Offset + Length > FileSize) {
        Length = FileSize - Offset;
    }

    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "\n--- %s: %llu bytes, showing 0x%llX..0x%llX ---\n\n",
                  Path, FileSize, Offset, Offset + Length);

    // We read and type line by line
    UINT8 LineBuf[HEXDUMP_LINE_BYTES];
    UINT64 Pos = Offset;
    UINT64 End = Offset + Length;

    while (Pos < End) {
        UINT32 ToRead = HEXDUMP_LINE_BYTES;
        if (Pos + ToRead > End) ToRead = (UINT32)(End - Pos);

        VfsSeek(File, Pos, SEEK_SET);
        UINT32 Got = 0;
        Rc = VfsRead(File, LineBuf, ToRead, &Got);
        if (Rc != SUCCESS || Got == 0) break;

        // Address
        BaseconSetColors(RGB_GRAY, RGB_BLACK);
        CHAR AddrBuf[32];
        SnPrintf(AddrBuf, sizeof(AddrBuf), "%08llX  ", Pos);
        BaseconPutString(AddrBuf);
        BaseconSetColors(RGB_GRAY, RGB_BLACK);

        // Hex bytes
        for (UINT32 I = 0; I < HEXDUMP_LINE_BYTES; I++) {
            if (I < Got) {
                PutHex8(LineBuf[I]);
            } else {
                BaseconPutString("  ");
            }
            if (I == 7) BaseconPutString(" ");   // the space between the halves
            if (I != HEXDUMP_LINE_BYTES - 1) BaseconPutString(" ");
        }

        BaseconPutString("  |");

        // ASCII
        for (UINT32 I = 0; I < Got; I++) {
            PutAscii(LineBuf[I]);
        }

        BaseconPutString("|\n");

        Pos += Got;
        if (Got < ToRead) break;
    }

    BaseconPutString("\n");

    VfsClose(File);
    return 0;
}

INT CmdHexDumpRegister(VOID) {
    return CmdRegisterBuiltin("hexdump",
                              "Hex dump of a file",
                              CmdHexDumpHandler);
}