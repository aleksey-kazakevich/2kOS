#include <Drivers/Disk/DiskMgr.h>
#include <Mem/Allocator.h>
#include <Io/StdWarn.h>
#include <Lib/String.h>
#include <Cmd/Format.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <Lib/StdIo.h>
#include <Fs/Exfat.h>
#include <Fs/Fat32.h>
#include <Return.h>

STATIC VOID CmdFormatPrintHelp(VOID) {
    StdOutPrintf("\n");
    StdOutPrintf("Usage: format <disk> <type>\n");
    StdOutPrintf("\n");
    StdOutPrintf("  disk   - Disk or partition (e.g., SATA0, SATA0:2)\n");
    StdOutPrintf("  type   - Filesystem type:\n");
    StdOutPrintf("           exfat fat32\n");
    StdOutPrintf("\n");
    StdOutPrintf("Examples:\n");
    StdOutPrintf("  format IDE0 exfat\n");
    StdOutPrintf("\n");
    StdWarnPrintf("WARNING: This will DESTROY ALL DATA on the disk!\n");
    StdOutPrintf("\n");
}

STATIC BOOL FormatIsKnownFs(const CHAR *FsType) {
    return StrCaseCmp(FsType, "exfat") == 0 ||
           StrCaseCmp(FsType, "fat32") == 0;
}

INT CmdFormatHandler(INT ArgC, const CHAR *ArgV[]) {
    const CHAR *DiskName;
    const CHAR *FsType;
    Disk *D;
    UINT64 StartLba = 0;
    UINT64 SectorCount = 0;
    INT Result;

    if (ArgC < 3) {
        CmdFormatPrintHelp();
        return 0;
    }

    DiskName = ArgV[1];
    FsType = ArgV[2];

    if (!FormatIsKnownFs(FsType)) {
        StdErrPrintf("Unsupported filesystem: %s\n", FsType);
        StdOutPrintf("Supported: exfat, fat32\n");
        return -1;
    }

    /* Disk or partition? Accept both "SATA0:2" and "SATA0p2". */
    Partition *Part = NULLPTR;
    if (StrChr(DiskName, ':') || StrrChr(DiskName, 'p')) {
        Part = DiskMgrFindPartition(DiskName);
    }

    if (Part) {
        D = Part->ParentDisk;
        StartLba = Part->StartLba;
        SectorCount = Part->SectorCount;
    } else {
        D = DiskMgrFindDisk(DiskName);
        StartLba = 0;
        SectorCount = D ? D->SectorCount : 0;
    }

    if (!D) {
        StdErrPrintf("Disk not found: %s\n", DiskName);
        return -1;
    }

    /* Refuse to format a mounted volume. */
    CHAR CheckPath[80];
    SnPrintf(CheckPath, sizeof(CheckPath), "%s::/", D->Name);
    VfsInode *CheckRoot;
    if (VfsWalk(VfsRootInode, CheckPath, &CheckRoot) == 0) {
        VfsInodeUnref(CheckRoot);
        StdErrPrintf("Drive %s is currently mounted!\n", D->Name);
        StdOutPrintf("Unmount it first: unmount %s\n", D->Name);
        return -1;
    }

    StdOutPrintf("\n");
    StdOutPrintf("Formatting %s as %s...\n", D->Name, FsType);
    StdOutPrintf("This may take a moment...\n");
    StdOutPrintf("\n");

    if (StrCaseCmp(FsType, "exfat") == 0) {
        Result = ExfatFormat(D, StartLba, SectorCount);
    } else if (StrCaseCmp(FsType, "fat32") == 0) {
        Result = Fat32Format(D, StartLba, SectorCount);
    } else {
        StdErrPrintf("Unsupported filesystem: %s\n", FsType);
        return -1;
    }

    if (Result != SUCCESS) {
        StdErrPrintf("Format failed (%d): %s\n",
                     Result, ReturnCode2String(Result));
        return -1;
    }

    StdOutPrintf("\n");
    StdOutPrintf("Format complete!\n");
    StdOutPrintf("\n");
    StdOutPrintf("To mount the disk: mount %s %s\n", D->Name, FsType);

    return 0;
}

INT CmdFormatRegister(VOID) {
    return CmdRegisterBuiltin("format", "Format a disk", CmdFormatHandler);
}