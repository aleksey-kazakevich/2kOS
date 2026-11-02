#include <Drivers/Disk/DiskMgr.h>
#include <Drivers/Disk/IsoFile.h>
#include <Lib/String.h>
#include <Io/StdWarn.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <Cmd/Mount.h>
#include <Lib/StdIo.h>
#include <Fs/Exfat.h>
#include <Fs/Vfs.h>
#include <Return.h>

STATIC VOID CmdMountPrintHelp(VOID) {
    StdOutPrintf("\n");
    StdOutPrintf("Usage: mount <disk|partition> [<type>]\n");
    StdOutPrintf("\n");
    StdOutPrintf("  disk       - Disk name (e.g., SATA0)\n");
    StdOutPrintf("  partition  - Partition name (e.g., SATA0:1, SATA0p1)\n");
    StdOutPrintf("  type       - Filesystem type (optional): exfat, fat32, iso9660, auto\n");
    StdOutPrintf("\n");
    StdOutPrintf("Examples:\n");
    StdOutPrintf("  mount SATA0        - Mount whole disk\n");
    StdOutPrintf("  mount SATA0:1      - Mount partition 1 (MBR)\n");
    StdOutPrintf("  mount NVME1p2      - Mount partition 2 (GPT)\n");
    StdOutPrintf("  mount SATA0:1 exfat\n");
    StdOutPrintf("\n");
}

/* Разбирает имя на "диск" + индекс раздела.
 * Возвращает TRUE, если имя похоже на раздел (SATA0:1 или SATA0p1). */
STATIC BOOL CmdMountParsePartition(const CHAR *Name,
                                   CHAR *DiskName, USIZE DiskNameSz,
                                   UINT32 *OutIndex) {
    if (!Name || !DiskName || DiskNameSz == 0) return FALSE;

    const CHAR *Sep = StrChr(Name, ':');
    if (Sep) {
        USIZE Len = (USIZE)(Sep - Name);
        if (Len == 0 || Len >= DiskNameSz) return FALSE;
        StrnCpy(DiskName, Name, Len);
        DiskName[Len] = '\0';
        *OutIndex = (UINT32)AToI(Sep + 1);
        return (*OutIndex > 0);
    }

    const CHAR *P = StrrChr(Name, 'p');
    if (P && P != Name && P[1] >= '0' && P[1] <= '9') {
        USIZE Len = (USIZE)(P - Name);
        if (Len == 0 || Len >= DiskNameSz) return FALSE;
        StrnCpy(DiskName, Name, Len);
        DiskName[Len] = '\0';
        *OutIndex = (UINT32)AToI(P + 1);
        return (*OutIndex > 0);
    }

    return FALSE;
}

/* Ищет точку монтирования по VfsInode (root) и возвращает её имя,
 * чтобы проверить, не занято ли уже. */
STATIC BOOL CmdMountIsMounted(const CHAR *MountName) {
    if (!MountName || !VfsRootInode) return FALSE;
    CHAR Path[96];
    SnPrintf(Path, sizeof(Path), "%s::/", MountName);
    VfsInode *Tmp = NULLPTR;
    if (VfsWalk(VfsRootInode, Path, &Tmp) == 0) {
        VfsInodeUnref(Tmp);
        return TRUE;
    }
    return FALSE;
}

INT CmdMountHandler(INT ArgC, const CHAR *ArgV[]) {
    const CHAR *Name;
    const CHAR *FsType;
    Disk *D = NULLPTR;
    Partition *Part = NULLPTR;
    UINT64 StartLba = 0;
    UINT64 SectorCount = 0;
    VfsInode *Root = NULLPTR;
    INT Result;
    CHAR MountedList[256];

    // No arguments — show a list
    if (ArgC < 2) {
        if (VfsListMountedDrives(MountedList, sizeof(MountedList)) > 0) {
            StdOutPrintf("\nMounted drives:\n  %s\n\n", MountedList);
        } else {
            StdOutPrintf("\nNo drives mounted.\n");
        }
        CmdMountPrintHelp();
        return 0;
    }

    Name = ArgV[1];
    FsType = (ArgC >= 3) ? ArgV[2] : NULLPTR;

    USIZE NameLen = StrLen(Name);
    BOOL LooksLikeFile = (StrChr(Name, ':') == NULLPTR) &&
                         (StrChr(Name, '/') != NULLPTR || StrChr(Name, '.') != NULLPTR);
    if (LooksLikeFile && NameLen > 4) {
        const CHAR *Ext = Name + NameLen - 4;
        if (StrCaseCmp(Ext, ".iso") == 0) {
            INT Rc = IsoMountFromFile(VfsCurrentDir, Name, FsType);
            if (Rc != SUCCESS) {
                StdErrPrintf("mount: cannot mount ISO '%s' (%d)\n", Name, Rc);
                return -1;
            }
            StdOutPrintf("Mounted ISO '%s'\n", Name);
            return 0;
        }
    }

    /* --- 1. Раздел или диск? --- */
    CHAR DiskName[DRIVE_NAME_MAX];
    UINT32 PartIndex = 0;

    if (CmdMountParsePartition(Name, DiskName, sizeof(DiskName), &PartIndex)) {
        /* Похоже на раздел. Пробуем найти его по полному имени. */
        Part = DiskMgrFindPartition(Name);
        if (!Part) {
            StdErrPrintf("Partition not found: %s\n", Name);
            StdOutPrintf("Use 'diskmgr parts <disk>' to list partitions.\n");
            return -1;
        }
        D = Part->ParentDisk;
        StartLba = Part->StartLba;
        SectorCount = Part->SectorCount;
        StdOutPrintf("Partition %s: start LBA %llu, %llu sectors\n",
                      Part->Name, StartLba, SectorCount);
    } else {
        /* Раздел не указан — монтируем весь диск. */
        D = DiskMgrFindDisk(Name);
        if (!D) {
            /* Может быть, это алиас раздела? (disk.cfg) */
            Part = DiskMgrFindPartition(Name);
            if (Part) {
                D = Part->ParentDisk;
                StartLba = Part->StartLba;
                SectorCount = Part->SectorCount;
                StdOutPrintf("Partition (alias) %s: start LBA %llu, %llu sectors\n",
                              Part->Name, StartLba, SectorCount);
            }
        }
        if (!D) {
            StdErrPrintf("Disk not found: %s\n", Name);
            StdOutPrintf("Available disks:\n");
            Disk *Tmp = DiskMgrGetFirst();
            while (Tmp) {
                StdOutPrintf("  %s (%llu MB)\n",
                              Tmp->Name, Tmp->TotalSize / (1024 * 1024));
                Tmp = DiskMgrGetNext(Tmp);
            }
            return -1;
        }
        if (!Part) {
            StartLba = 0;
            SectorCount = D->SectorCount;
        }
    }

    /* --- 2. Точка монтирования --- */
    /* Для раздела — его полное имя (SATA0:1 / NVME1p2),
     * для диска — имя диска. Алиас (из disk.cfg) — приоритетнее. */
    CHAR MountName[DRIVE_NAME_MAX];
    if (Part) {
        if (Part->Alias[0]) {
            StrnCpy(MountName, Part->Alias, sizeof(MountName) - 1);
        } else {
            StrnCpy(MountName, Part->Name, sizeof(MountName) - 1);
        }
    } else {
        if (D->Alias[0]) {
            StrnCpy(MountName, D->Alias, sizeof(MountName) - 1);
        } else {
            StrnCpy(MountName, D->Name, sizeof(MountName) - 1);
        }
    }
    MountName[sizeof(MountName) - 1] = '\0';

    /* --- 3. Проверка «уже смонтировано» по РЕАЛЬНОЙ точке --- */
    if (CmdMountIsMounted(MountName)) {
        StdWarnPrintf("Already mounted: %s\n", MountName);
        return -1;
    }

    /* Дополнительно: если это раздел, а на его LBA уже что-то висит
     * (например, смонтировали весь диск SATA0, а теперь SATA0:1) —
     * VfsMountEx отклонит по (Dev, StartLba). Сообщим понятнее. */
    if (Part) {
        CHAR ParentMount[DRIVE_NAME_MAX];
        StrnCpy(ParentMount, D->Name, sizeof(ParentMount) - 1);
        ParentMount[sizeof(ParentMount) - 1] = '\0';
        if (CmdMountIsMounted(ParentMount)) {
            StdWarnPrintf("%s is mounted as a whole disk. "
                          "Unmount it before mounting a partition.\n",
                          ParentMount);
            return -1;
        }
    }

    /* --- 4. Монтируем --- */
    StdOutPrintf("Mounting %s (as %s)...\n", Name, MountName);

    /* Используем VfsMountEx, чтобы зафиксировать точку монтирования
     * (VfsMount взял бы Dev->Name). */
    Result = VfsMountEx(FsType, D, StartLba, SectorCount, MountName, &Root);
    if (Result != 0) {
        StdErrPrintf("Mount failed (error %d)\n", Result);
        return -1;
    }

    StdOutPrintf("Mounted %s successfully\n", MountName);

    VfsInodeUnref(Root);
    return 0;
}

INT CmdMountRegister(VOID) {
    return CmdRegisterBuiltin("mount", "Mount a disk or partition", CmdMountHandler);
}