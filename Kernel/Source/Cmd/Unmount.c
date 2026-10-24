#include <Drivers/Disk/DiskMgr.h>
#include <Cmd/Unmount.h>
#include <Lib/String.h>
#include <Io/StdWarn.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <Lib/StdIo.h>
#include <Fs/Vfs.h>
#include <Return.h>

STATIC VOID CmdUnmountPrintHelp(VOID) {
    StdOutPrintf("\n");
    StdOutPrintf("Usage: unmount <disk>\n");
    StdOutPrintf("\n");
    StdOutPrintf("  disk   - Disk name (e.g., SATA0, IDE0)\n");
    StdOutPrintf("\n");
    StdOutPrintf("Example:\n");
    StdOutPrintf("  unmount SATA0\n");
    StdOutPrintf("\n");
}

INT CmdUnmountHandler(INT ArgC, const CHAR *ArgV[]) {
    const CHAR *DiskName;
    Disk *D;
    CHAR MountedList[256];
    INT Result;

    // Checking the arguments
    if (ArgC < 2) {
        // If there are no arguments, we show a list of mounted disks.
        if (VfsListMountedDrives(MountedList, sizeof(MountedList)) > 0) {
            StdOutPrintf("\n");
            StdOutPrintf("Mounted drives:\n");
            StdOutPrintf("  %s\n", MountedList);
            StdOutPrintf("\n");
        } else {
            StdOutPrintf("\n");
            StdOutPrintf("No drives mounted.\n");
        }
        CmdUnmountPrintHelp();
        return 0;
    }

    DiskName = ArgV[1];

    // Looking for a disk
    D = DiskMgrFindDisk(DiskName);
    if (!D) {
        StdErrPrintf("Disk not found: %s\n", DiskName);
        return -1;
    }

    // Checking if the disk is mounted
    CHAR CheckPath[64];
    SnPrintf(CheckPath, sizeof(CheckPath), "%s::/", D->Name);
    VfsInode *CheckRoot;
    if (VfsWalk(VfsRootInode, CheckPath, &CheckRoot) != 0) {
        StdWarnPrintf("Drive %s is not mounted\n", D->Name);
        return -1;
    }
    VfsInodeUnref(CheckRoot);

    // We're checking to see if we're inside this disk.
    if (VfsCurrentDir) {
        CHAR CurrentPath[PATH_MAX];
        if (VfsBuildPath(VfsCurrentDir, CurrentPath, PATH_MAX) == 0) {
            if (StrStr(CurrentPath, DiskName) != NULLPTR) {
                StdWarnPrintf("You are inside %s. Changing to root...\n", DiskName);
                VfsCd(&VfsCurrentDir, "/");
            }
        }
    }

    // Unmount it
    StdOutPrintf("Unmounting %s...\n", D->Name);
    
    Result = VfsUnmountDrive(D->Name);
    if (Result != 0) {
        StdErrPrintf("Unmount failed (error %d)\n", Result);
        StdOutPrintf("Make sure no files are open on this drive\n");
        return -1;
    }

    StdOutPrintf("Unmounted %s successfully\n", D->Name);
    return 0;
}

INT CmdUnmountRegister(VOID) {
    return CmdRegisterBuiltin("unmount", "Unmount a disk", CmdUnmountHandler);
}