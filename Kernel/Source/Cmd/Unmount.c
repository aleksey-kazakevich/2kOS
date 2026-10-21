#include <Drivers/Disk/DiskMgr.h>
#include <Cmd/Unmount.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Basecon.h>
#include <Fs/Vfs.h>
#include <Return.h>

STATIC VOID CmdUnmountPrintHelp(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Usage: unmount <disk>\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  disk   - Disk name (e.g., SATA0, IDE0)\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Example:\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  unmount SATA0\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
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
            BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
            BaseconPrintf(BASECON_TYPE_NORMAL, "Mounted drives:\n");
            BaseconPrintf(BASECON_TYPE_NORMAL, "  %s\n", MountedList);
            BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
        } else {
            BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
            BaseconPrintf(BASECON_TYPE_NORMAL, "No drives mounted.\n");
        }
        CmdUnmountPrintHelp();
        return 0;
    }

    DiskName = ArgV[1];

    // Looking for a disk
    D = DiskMgrFindDisk(DiskName);
    if (!D) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Disk not found: %s\n", DiskName);
        return -1;
    }

    // Checking if the disk is mounted
    CHAR CheckPath[64];
    SnPrintf(CheckPath, sizeof(CheckPath), "%s::/", D->Name);
    VfsInode *CheckRoot;
    if (VfsWalk(VfsRootInode, CheckPath, &CheckRoot) != 0) {
        BaseconPrintf(BASECON_TYPE_WARNING, "Drive %s is not mounted\n", D->Name);
        return -1;
    }
    VfsInodeUnref(CheckRoot);

    // We're checking to see if we're inside this disk.
    if (VfsCurrentDir) {
        CHAR CurrentPath[PATH_MAX];
        if (VfsBuildPath(VfsCurrentDir, CurrentPath, PATH_MAX) == 0) {
            if (StrStr(CurrentPath, DiskName) != NULLPTR) {
                BaseconPrintf(BASECON_TYPE_WARNING, "You are inside %s. Changing to root...\n", DiskName);
                VfsCd(&VfsCurrentDir, "/");
            }
        }
    }

    // Unmount it
    BaseconPrintf(BASECON_TYPE_NORMAL, "Unmounting %s...\n", D->Name);
    
    Result = VfsUnmountDrive(D->Name);
    if (Result != 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Unmount failed (error %d)\n", Result);
        BaseconPrintf(BASECON_TYPE_NORMAL, "Make sure no files are open on this drive\n");
        return -1;
    }

    BaseconPrintf(BASECON_TYPE_SUCCESS, "Unmounted %s successfully\n", D->Name);
    return 0;
}

INT CmdUnmountRegister(VOID) {
    return CmdRegisterBuiltin("unmount", "Unmount a disk", CmdUnmountHandler);
}