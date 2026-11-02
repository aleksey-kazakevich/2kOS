#include <Drivers/Disk/RamDisk.h>
#include <Drivers/Disk/DiskMgr.h>
#include <Cmd/RamDisk.h>
#include <Lib/String.h>
#include <Io/StdWarn.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <Lib/StdIo.h>
#include <Fs/Vfs.h>
#include <Return.h>
#include <Rgb.h>

// ============================================================================
// Help
// ============================================================================

STATIC VOID CmdRamDiskHelp(VOID) {
    StdOutPrintf("\n");
    StdOutPrintf("Usage:\n");
    StdOutPrintf("  ramdisk create <name> <size_mb>\n");
    StdOutPrintf("  ramdisk destroy <name>\n");
    StdOutPrintf("  ramdisk list\n");
    StdOutPrintf("\n");
    StdOutPrintf("Examples:\n");
    StdOutPrintf("  ramdisk create RAM0 64\n");
    StdOutPrintf("  ramdisk list\n");
    StdOutPrintf("  ramdisk destroy RAM0\n");
    StdOutPrintf("\n");
}

// ============================================================================
// create
// ============================================================================

STATIC INT CmdRamDiskCreate(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 4) {
        StdErrPrintf("Usage: ramdisk create <name> <size_mb>\n");
        return INCORRECT_VALUE;
    }

    const CHAR *Name = ArgV[2];
    INT SizeMB = AToI(ArgV[3]);

    if (SizeMB <= 0) {
        StdErrPrintf("Invalid size: %s\n", ArgV[3]);
        return INCORRECT_VALUE;
    }

    if (SizeMB > 4096) {
        StdErrPrintf("Too large (max 4096 MB)\n");
        return INCORRECT_VALUE;
    }

    // Checking to see if there is already such a name.
    if (DiskMgrFindDisk(Name)) {
        StdErrPrintf("Already exists: %s\n", Name);
        return ALREADY_EXISTS;
    }

    UINT64 SizeBytes = (UINT64)SizeMB * 1024 * 1024;
    Disk *D = RamDiskCreate(Name, SizeBytes);
    if (!D) {
        StdErrPrintf("Failed to create RAMDisk\n");
        return NO_MEMORY;
    }

    return SUCCESS;
}

// ============================================================================
// destroy
// ============================================================================

STATIC INT CmdRamDiskDestroy(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 3) {
        StdErrPrintf("Usage: ramdisk destroy <name>\n");
        return INCORRECT_VALUE;
    }

    const CHAR *Name = ArgV[2];

    Disk *D = DiskMgrFindDisk(Name);
    if (!D) {
        StdErrPrintf("Not found: %s\n", Name);
        return NO_OBJECT;
    }

    if (D->Type != DISK_TYPE_RAM) {
        StdErrPrintf("%s is not a RAMDisk\n", Name);
        return INCORRECT_VALUE;
    }

    // We will unmount it if it is mounted
    CHAR CheckPath[80];
    SnPrintf(CheckPath, sizeof(CheckPath), "%s::/", Name);
    VfsInode *CheckRoot;
    if (VfsWalk(VfsRootInode, CheckPath, &CheckRoot) == 0) {
        VfsInodeUnref(CheckRoot);
        StdWarnPrintf("Unmounting %s first...\n", Name);
        VfsUnmountDrive(Name);
    }

    // Deleting it from DiskMgr
    INT Ret = DiskMgrUnregisterDisk(D);
    if (Ret != SUCCESS) {
        StdErrPrintf("Unregister failed (%d)\n", Ret);
        return Ret;
    }

    // Freeing up memory
    RamDiskDestroy(D);

    StdOutPrintf("Destroyed: %s\n", Name);
    return SUCCESS;
}

// ============================================================================
// list
// ============================================================================

STATIC INT CmdRamDiskList(VOID) {
    StdOutPrintf("\n");
    StdOutPrintf("RAM disks:\n");

    INT Count = 0;
    Disk *D = DiskMgrGetFirst();
    while (D) {
        if (D->Type == DISK_TYPE_RAM) {
            UINT64 SizeMB = D->TotalSize / (1024 * 1024);
            StdOutPrintf("  %-10s  %llu MB  (%llu sectors)\n",
                          D->Name, SizeMB, D->SectorCount);
            Count++;
        }
        D = DiskMgrGetNext(D);
    }

    if (Count == 0) {
        StdOutPrintf("  (none)\n");
    }

    StdOutPrintf("\n");
    StdOutPrintf("Total: %d RAMDisk(s)\n", Count);
    StdOutPrintf("\n");
    return SUCCESS;
}

// ============================================================================
// Handler
// ============================================================================

INT CmdRamDiskHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        CmdRamDiskHelp();
        return INCORRECT_VALUE;
    }

    const CHAR *Sub = ArgV[1];

    if (StrCmp(Sub, "create") == 0) {
        return CmdRamDiskCreate(ArgC, ArgV);
    }
    if (StrCmp(Sub, "destroy") == 0 || StrCmp(Sub, "del") == 0) {
        return CmdRamDiskDestroy(ArgC, ArgV);
    }
    if (StrCmp(Sub, "list") == 0 || StrCmp(Sub, "ls") == 0) {
        return CmdRamDiskList();
    }

    StdErrPrintf("Unknown subcommand: %s\n", Sub);
    CmdRamDiskHelp();
    return INCORRECT_VALUE;
}

INT CmdRamDiskRegister(VOID) {
    return CmdRegisterBuiltin("ramdisk",
                              "Manage RAM disks",
                              CmdRamDiskHandler);
}