#include <Drivers/Disk/DiskMgr.h>
#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Cmd/Format.h>
#include <Lib/StdIo.h>
#include <Fs/Exfat.h>
#include <Basecon.h>
#include <Return.h>

STATIC VOID CmdFormatPrintHelp(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Usage: format <disk> <type>\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  disk   - Disk name (e.g., SATA0, IDE0)\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  type   - Filesystem type:\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "           exfat\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Example:\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  format IDE0 exfat\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_WARNING, "WARNING: This will DESTROY ALL DATA on the disk!\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
}

INT CmdFormatHandler(INT ArgC, const CHAR *ArgV[]) {
    const CHAR *DiskName;
    const CHAR *FsType;
    Disk *D;
    UINT64 StartLba = 0;
    UINT64 SectorCount = 0;
    INT Result;

    // Проверяем аргументы
    if (ArgC < 3) {
        CmdFormatPrintHelp();
        return 0;
    }

    DiskName = ArgV[1];
    FsType = ArgV[2];

    // Проверяем тип ФС
    if (StrCaseCmp(FsType, "exfat") != 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Unsupported filesystem: %s\n", FsType);
        BaseconPrintf(BASECON_TYPE_NORMAL, "Supported: exfat\n");
        return -1;
    }

    // Ищем диск
    Partition *Part = NULLPTR;
    if (StrChr(DiskName, ':')) {
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
        BaseconPrintf(BASECON_TYPE_ERROR, "Disk not found: %s\n", DiskName);
        return -1;
    }

    if (!D) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Disk not found: %s\n", DiskName);
        BaseconPrintf(BASECON_TYPE_NORMAL, "Available disks:\n");
        
        Disk *Tmp = DiskMgrGetFirst();
        while (Tmp) {
            BaseconPrintf(BASECON_TYPE_NORMAL, "  %s (%llu MB)\n", 
                          Tmp->Name, Tmp->TotalSize / (1024 * 1024));
            Tmp = DiskMgrGetNext(Tmp);
        }
        return -1;
    }

    // Проверяем, не смонтирован ли диск
    CHAR CheckPath[64];
    SnPrintf(CheckPath, sizeof(CheckPath), "%s::/", D->Name);
    VfsInode *CheckRoot;
    if (VfsWalk(VfsRootInode, CheckPath, &CheckRoot) == 0) {
        VfsInodeUnref(CheckRoot);
        BaseconPrintf(BASECON_TYPE_ERROR, "Drive %s is currently mounted!\n", D->Name);
        BaseconPrintf(BASECON_TYPE_NORMAL, "Unmount it first: unmount %s\n", D->Name);
        return -1;
    }

    // Форматируем
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Formatting %s as %s...\n", D->Name, FsType);
    BaseconPrintf(BASECON_TYPE_NORMAL, "This may take a moment...\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");

    if (StrCaseCmp(FsType, "exfat") == 0) {
        Result = ExfatFormat(D, StartLba, SectorCount);
    } else {
        BaseconPrintf(BASECON_TYPE_ERROR, "Unsupported filesystem: %s\n", FsType);
        return -1;
    }

    if (Result != 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Format failed (error %d)\n", Result);
        return -1;
    }

    BaseconPrintf(BASECON_TYPE_SUCCESS, "\n");
    BaseconPrintf(BASECON_TYPE_SUCCESS, "Format complete!\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "To mount the disk: mount %s\n", D->Name);
    
    return 0;
}

INT CmdFormatRegister(VOID) {
    return CmdRegisterBuiltin("format", "Format a disk", CmdFormatHandler);
}