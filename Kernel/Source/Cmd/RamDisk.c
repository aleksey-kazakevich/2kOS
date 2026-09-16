#include <Drivers/Disk/RamDisk.h>
#include <Drivers/Disk/DiskMgr.h>
#include <Cmd/RamDisk.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Basecon.h>
#include <Fs/Vfs.h>
#include <Return.h>
#include <Rgb.h>

// ============================================================================
// Help
// ============================================================================

STATIC VOID CmdRamDiskHelp(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Usage:\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  ramdisk create <name> <size_mb>\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  ramdisk destroy <name>\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  ramdisk list\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Examples:\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  ramdisk create RAM0 64\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  ramdisk list\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  ramdisk destroy RAM0\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
}

// ============================================================================
// create
// ============================================================================

STATIC INT CmdRamDiskCreate(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 4) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Usage: ramdisk create <name> <size_mb>\n");
        return INCORRECT_VALUE;
    }

    const CHAR *Name = ArgV[2];
    INT SizeMB = AToI(ArgV[3]);

    if (SizeMB <= 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Invalid size: %s\n", ArgV[3]);
        return INCORRECT_VALUE;
    }

    if (SizeMB > 4096) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Too large (max 4096 MB)\n");
        return INCORRECT_VALUE;
    }

    // Проверяем, нет ли уже такого имени
    if (DiskMgrFindDisk(Name)) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Already exists: %s\n", Name);
        return ALREADY_EXISTS;
    }

    UINT64 SizeBytes = (UINT64)SizeMB * 1024 * 1024;
    Disk *D = RamDiskCreate(Name, SizeBytes);
    if (!D) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Failed to create RAMDisk\n");
        return NO_MEMORY;
    }

    return SUCCESS;
}

// ============================================================================
// destroy
// ============================================================================

STATIC INT CmdRamDiskDestroy(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 3) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Usage: ramdisk destroy <name>\n");
        return INCORRECT_VALUE;
    }

    const CHAR *Name = ArgV[2];

    Disk *D = DiskMgrFindDisk(Name);
    if (!D) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Not found: %s\n", Name);
        return NO_OBJECT;
    }

    if (D->Type != DISK_TYPE_RAM) {
        BaseconPrintf(BASECON_TYPE_ERROR, "%s is not a RAMDisk\n", Name);
        return INCORRECT_VALUE;
    }

    // Отмонтируем, если смонтирован
    CHAR CheckPath[80];
    SnPrintf(CheckPath, sizeof(CheckPath), "%s::/", Name);
    VfsInode *CheckRoot;
    if (VfsWalk(VfsRootInode, CheckPath, &CheckRoot) == 0) {
        VfsInodeUnref(CheckRoot);
        BaseconPrintf(BASECON_TYPE_WARNING, "Unmounting %s first...\n", Name);
        VfsUnmountDrive(Name);
    }

    // Удаляем из DiskMgr
    INT Ret = DiskMgrUnregisterDisk(D);
    if (Ret != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Unregister failed (%d)\n", Ret);
        return Ret;
    }

    // Освобождаем память
    RamDiskDestroy(D);

    BaseconPrintf(BASECON_TYPE_SUCCESS, "Destroyed: %s\n", Name);
    return SUCCESS;
}

// ============================================================================
// list
// ============================================================================

STATIC INT CmdRamDiskList(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "RAM disks:\n");

    INT Count = 0;
    Disk *D = DiskMgrGetFirst();
    while (D) {
        if (D->Type == DISK_TYPE_RAM) {
            UINT64 SizeMB = D->TotalSize / (1024 * 1024);
            BaseconPrintf(BASECON_TYPE_NORMAL,
                          "  %-10s  %llu MB  (%llu sectors)\n",
                          D->Name, SizeMB, D->SectorCount);
            Count++;
        }
        D = DiskMgrGetNext(D);
    }

    if (Count == 0) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "  (none)\n");
    }

    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Total: %d RAMDisk(s)\n", Count);
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
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

    BaseconPrintf(BASECON_TYPE_ERROR, "Unknown subcommand: %s\n", Sub);
    CmdRamDiskHelp();
    return INCORRECT_VALUE;
}

INT CmdRamDiskRegister(VOID) {
    return CmdRegisterBuiltin("ramdisk",
                              "Manage RAM disks",
                              CmdRamDiskHandler);
}