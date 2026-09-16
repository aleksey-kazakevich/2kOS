#include <Drivers/Disk/DiskMgr.h>
#include <Lib/String.h>
#include <Cmd/Mount.h>
#include <Lib/StdIo.h>
#include <Fs/Exfat.h>
#include <Basecon.h>
#include <Fs/Vfs.h>
#include <Return.h>

STATIC VOID CmdMountPrintHelp(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Usage: mount <disk|partition> [<type>]\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  disk       - Disk name (e.g., SATA0)\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  partition  - Partition name (e.g., SATA0:1)\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  type       - Filesystem type (optional): exfat, auto\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Examples:\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  mount SATA0        - Mount whole disk\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  mount SATA0:1      - Mount partition 1\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  mount SATA0:1 exfat\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
}

INT CmdMountHandler(INT ArgC, const CHAR *ArgV[]) {
    const CHAR *Name;
    const CHAR *FsType;
    Disk *D = NULLPTR;
    UINT64 StartLba = 0;
    UINT64 SectorCount = 0;
    VfsInode *Root;
    INT Result;
    CHAR MountedList[256];

    // Нет аргументов — показать список
    if (ArgC < 2) {
        if (VfsListMountedDrives(MountedList, sizeof(MountedList)) > 0) {
            BaseconPrintf(BASECON_TYPE_NORMAL, "\nMounted drives:\n  %s\n\n", MountedList);
        } else {
            BaseconPrintf(BASECON_TYPE_NORMAL, "\nNo drives mounted.\n");
        }
        CmdMountPrintHelp();
        return 0;
    }

    Name = ArgV[1];
    FsType = (ArgC >= 3) ? ArgV[2] : NULLPTR;

    // ─── Определяем: диск или раздел? ───
    if (StrChr(Name, ':')) {
        // Раздел
        Partition *Part = DiskMgrFindPartition(Name);
        if (!Part) {
            BaseconPrintf(BASECON_TYPE_ERROR, "Partition not found: %s\n", Name);
            return -1;
        }
        D = Part->ParentDisk;
        StartLba = Part->StartLba;
        SectorCount = Part->SectorCount;
        BaseconPrintf(BASECON_TYPE_NORMAL, "Partition %s: start LBA %llu, %llu sectors\n",
                      Part->Name, StartLba, SectorCount);
    } else {
        // Целый диск
        D = DiskMgrFindDisk(Name);
        if (!D) {
            BaseconPrintf(BASECON_TYPE_ERROR, "Disk not found: %s\n", Name);
            BaseconPrintf(BASECON_TYPE_NORMAL, "Available disks:\n");
            Disk *Tmp = DiskMgrGetFirst();
            while (Tmp) {
                BaseconPrintf(BASECON_TYPE_NORMAL, "  %s (%llu MB)\n",
                              Tmp->Name, Tmp->TotalSize / (1024 * 1024));
                Tmp = DiskMgrGetNext(Tmp);
            }
            return -1;
        }
        StartLba = 0;
        SectorCount = D->SectorCount;
    }

    // Проверка на уже смонтированный (по имени)
    CHAR CheckPath[80];
    SnPrintf(CheckPath, sizeof(CheckPath), "%s::/", Name);
    VfsInode *CheckRoot;
    if (VfsWalk(VfsRootInode, CheckPath, &CheckRoot) == 0) {
        VfsInodeUnref(CheckRoot);
        BaseconPrintf(BASECON_TYPE_WARNING, "Already mounted: %s\n", Name);
        return -1;
    }

    // ─── Монтируем ───
    BaseconPrintf(BASECON_TYPE_NORMAL, "Mounting %s...\n", Name);

    Result = VfsMount(FsType, D, StartLba, SectorCount, &Root);
    if (Result != 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Mount failed (error %d)\n", Result);
        return -1;
    }

    BaseconPrintf(BASECON_TYPE_SUCCESS, "Mounted %s successfully\n", Name);

    VfsInodeUnref(Root);
    return 0;
}

INT CmdMountRegister(VOID) {
    return CmdRegisterBuiltin("mount", "Mount a disk or partition", CmdMountHandler);
}