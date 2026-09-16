#include <Drivers/Disk/DiskMgr.h>
#include <Drivers/Disk/Pata.h>
#include <Mem/Allocator.h>
#include <Cmd/DiskMgr.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Basecon.h>
#include <Return.h>

STATIC VOID CmdPrintDiskInfo(Disk *D) {
    if (!D) return;
    
    const CHAR *TypeStr = DiskTypeToString(D->Type);
    CHAR SizeStr[32];
    
    // Форматируем размер
    if (D->TotalSize > 1024 * 1024 * 1024) {
        SnPrintf(SizeStr, sizeof(SizeStr), "%.2f GB", 
                 (FLOAT)D->TotalSize / (1024 * 1024 * 1024));
    } else if (D->TotalSize > 1024 * 1024) {
        SnPrintf(SizeStr, sizeof(SizeStr), "%.2f MB", 
                 (FLOAT)D->TotalSize / (1024 * 1024));
    } else {
        SnPrintf(SizeStr, sizeof(SizeStr), "%llu B", D->TotalSize);
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "  %-8s %-8s %-12s %-10llu %s\n",
                  D->Name,
                  TypeStr,
                  SizeStr,
                  D->SectorCount,
                  D->Initialized ? "OK" : "FAIL");
}

STATIC VOID CmdPrintPartitionInfo(Partition *P) {
    if (!P) return;
    
    CHAR SizeStr[32];
    if (P->SizeInBytes > 1024 * 1024 * 1024) {
        SnPrintf(SizeStr, sizeof(SizeStr), "%.2f GB", 
                 (FLOAT)P->SizeInBytes / (1024 * 1024 * 1024));
    } else if (P->SizeInBytes > 1024 * 1024) {
        SnPrintf(SizeStr, sizeof(SizeStr), "%.2f MB", 
                 (FLOAT)P->SizeInBytes / (1024 * 1024));
    } else {
        SnPrintf(SizeStr, sizeof(SizeStr), "%llu B", P->SizeInBytes);
    }
    
    const CHAR *TypeStr = PartitionTypeToString(P->Type);
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "  %-12s %-12s %-10s 0x%02X %s\n",
                  P->Name,
                  SizeStr,
                  TypeStr,
                  P->PartitionId,
                  P->Bootable ? "BOOT" : "");
}

STATIC INT CmdDiskMgrList(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Available Disks:\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  NAME     TYPE     SIZE         SECTORS    STATUS\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  -------- -------- ------------ ---------- ------\n");
    
    Disk *D = DiskMgrGetFirst();
    if (!D) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "  No disks found\n");
        return 0;
    }
    
    while (D) {
        CmdPrintDiskInfo(D);
        D = DiskMgrGetNext(D);
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Total: %d disk(s)\n", DiskMgrGetDiskCount());
    return 0;
}

STATIC INT CmdDiskMgrInfo(const CHAR *DiskName) {
    if (!DiskName) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Usage: diskmgr info <disk>\n");
        return -1;
    }
    
    Disk *D = DiskMgrFindDisk(DiskName);
    if (!D) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Disk not found: %s\n", DiskName);
        return -1;
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "=== Disk Information: %s ===\n", D->Name);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  Type:          %s\n", DiskTypeToString(D->Type));
    BaseconPrintf(BASECON_TYPE_NORMAL, "  Total Size:    %llu bytes (%.2f GB)\n", 
                  D->TotalSize, (FLOAT)D->TotalSize / (1024 * 1024 * 1024));
    BaseconPrintf(BASECON_TYPE_NORMAL, "  Sectors:       %llu\n", D->SectorCount);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  Sector Size:   %u bytes\n", D->SectorSize);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  Partitions:    %u (primary), %u (logical)\n",
                  D->PartitionCount, D->LogicalCount);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  Initialized:   %s\n", D->Initialized ? "Yes" : "No");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    
    return 0;
}

STATIC INT CmdDiskMgrPartitions(const CHAR *DiskName) {
    if (!DiskName) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Usage: diskmgr parts <disk>\n");
        return -1;
    }
    
    Disk *D = DiskMgrFindDisk(DiskName);
    if (!D) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Disk not found: %s\n", DiskName);
        return -1;
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "=== Partitions on %s ===\n", D->Name);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  NAME         SIZE         TYPE       ID   BOOT\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  ------------ ------------ ---------- ---- ----\n");
    
    // Primary partitions
    for (UINT32 I = 0; I < D->PartitionCount; I++) {
        Partition *P = DiskMgrGetPartition(D, I);
        if (P) CmdPrintPartitionInfo(P);
    }
    
    // Logical partitions
    for (UINT32 I = 0; I < D->LogicalCount; I++) {
        Partition *P = DiskMgrGetLogical(D, I);
        if (P) CmdPrintPartitionInfo(P);
    }
    
    if (D->PartitionCount == 0 && D->LogicalCount == 0) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "  No partitions found\n");
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Total: %d partition(s)\n", 
                  D->PartitionCount + D->LogicalCount);
    return 0;
}

STATIC INT CmdDiskMgrCreate(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 4) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Usage: diskmgr create <disk> <size> <type>\n");
        BaseconPrintf(BASECON_TYPE_ERROR, "  size: in MB (e.g., 100)\n");
        BaseconPrintf(BASECON_TYPE_ERROR, "  type: FAT32, NTFS, Linux, EFI\n");
        return -1;
    }
    
    const CHAR *DiskName = ArgV[2];
    UINT64 SizeMB = AToI(ArgV[3]);
    
    // ============================================================
    // ПРЕОБРАЗУЕМ ИМЯ ТИПА В КОД
    // ============================================================
    const CHAR *TypeName = ArgV[4];
    UINT8 Type = 0;
    
    if (StrCaseCmp(TypeName, "FAT32") == 0 || StrCmp(TypeName, "fat32") == 0) {
        Type = 0x0B;
    } else if (StrCaseCmp(TypeName, "NTFS") == 0 || StrCmp(TypeName, "ntfs") == 0) {
        Type = 0x07;
    } else if (StrCaseCmp(TypeName, "Linux") == 0 || StrCmp(TypeName, "linux") == 0) {
        Type = 0x83;
    } else if (StrCaseCmp(TypeName, "EFI") == 0 || StrCmp(TypeName, "efi") == 0) {
        Type = 0xEF;
    } else if (StrCaseCmp(TypeName, "SWAP") == 0 || StrCmp(TypeName, "swap") == 0) {
        Type = 0x82;
    } else if (StrCaseCmp(TypeName, "FAT16") == 0 || StrCmp(TypeName, "fat16") == 0) {
        Type = 0x06;
    } else {
        // Если не распознали — пробуем как шестнадцатеричный код
        Type = AToHex(TypeName);
        if (Type == 0) {
            BaseconPrintf(BASECON_TYPE_ERROR, "Unknown partition type: %s\n", TypeName);
            BaseconPrintf(BASECON_TYPE_NORMAL, "Supported: FAT32, NTFS, Linux, EFI, SWAP, FAT16\n");
            return -1;
        }
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "Type: %s -> 0x%02X\n", TypeName, Type);
    
    Disk *D = DiskMgrFindDisk(DiskName);
    if (!D) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Disk not found: %s\n", DiskName);
        return -1;
    }
    
    // Проверяем, что размер не превышает диск
    UINT64 Sectors = (SizeMB * 1024 * 1024) / D->SectorSize;
    if (Sectors == 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Invalid size: %llu MB\n", SizeMB);
        return -1;
    }
    
    // Находим свободное место (простое решение — в конец диска)
    UINT64 StartLba = D->SectorCount - Sectors;
    
    // Проверяем, что есть место
    if (StartLba < 2048) {  // Минимальное смещение 1 MB
        BaseconPrintf(BASECON_TYPE_ERROR, "Not enough free space on disk\n");
        return -1;
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "Creating partition on %s: start=%llu, sectors=%llu\n",
                  D->Name, StartLba, Sectors);
    
    INT Result = PartitionCreateMBR(D, StartLba, Sectors, Type);
    if (Result != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Failed to create partition (error %d)\n", Result);
        return -1;
    }
    
    BaseconPrintf(BASECON_TYPE_SUCCESS, "Partition created successfully\n");
    return 0;
}

STATIC INT CmdDiskMgrDelete(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 3) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Usage: diskmgr delete <disk> <partition_index>\n");
        BaseconPrintf(BASECON_TYPE_ERROR, "  partition_index: 1-4 (primary)\n");
        return -1;
    }
    
    const CHAR *DiskName = ArgV[2];
    UINT32 PartIndex = AToI(ArgV[3]);
    
    Disk *D = DiskMgrFindDisk(DiskName);
    if (!D) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Disk not found: %s\n", DiskName);
        return -1;
    }
    
    if (PartIndex < 1 || PartIndex > 4) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Invalid partition index: %u (must be 1-4)\n", PartIndex);
        return -1;
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "Deleting partition %u from %s...\n", PartIndex, D->Name);
    
    INT Result = PartitionDeleteMBR(D, PartIndex);
    if (Result != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Failed to delete partition (error %d)\n", Result);
        return -1;
    }
    
    BaseconPrintf(BASECON_TYPE_SUCCESS, "Partition deleted successfully\n");
    return 0;
}

STATIC INT CmdDiskMgrHelp(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "=== DiskMgr Commands ===\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  diskmgr list              - List all disks\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  diskmgr info <disk>      - Show disk information\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  diskmgr parts <disk>     - List partitions on disk\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  diskmgr create <disk> <size> <type> - Create partition\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  diskmgr delete <disk> <idx> - Delete partition\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  diskmgr help             - Show this help\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Partition types:\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  NTFS FAT32 Linux EFI SWAP FAT16\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    return 0;
}

INT CmdDiskMgrHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        CmdDiskMgrHelp();
        return 0;
    }
    
    // ============================================================
    // НОРМАЛИЗУЕМ ПЕРВЫЙ АРГУМЕНТ (удаляем \r, \n, лишние пробелы)
    // ============================================================
    CHAR SubCmd[32];
    StrnCpy(SubCmd, ArgV[1], sizeof(SubCmd) - 1);
    SubCmd[sizeof(SubCmd) - 1] = '\0';
    
    // Удаляем \r и \n в конце
    USIZE Len = StrLen(SubCmd);
    while (Len > 0 && (SubCmd[Len-1] == '\r' || SubCmd[Len-1] == '\n' || SubCmd[Len-1] == ' ')) {
        SubCmd[--Len] = '\0';
    }
    
    // ============================================================
    // ТЕПЕРЬ СРАВНИВАЕМ НОРМАЛИЗОВАННУЮ СТРОКУ
    // ============================================================
    if (StrCmp(SubCmd, "list") == 0 || StrCmp(SubCmd, "ls") == 0) {
        return CmdDiskMgrList();
    }
    
    if (StrCmp(SubCmd, "info") == 0) {
        return CmdDiskMgrInfo(ArgC >= 3 ? ArgV[2] : NULLPTR);
    }
    
    if (StrCmp(SubCmd, "parts") == 0 || StrCmp(SubCmd, "partitions") == 0) {
        return CmdDiskMgrPartitions(ArgC >= 3 ? ArgV[2] : NULLPTR);
    }
    
    if (StrCmp(SubCmd, "create") == 0) {
        return CmdDiskMgrCreate(ArgC, ArgV);
    }
    
    if (StrCmp(SubCmd, "delete") == 0 || StrCmp(SubCmd, "del") == 0) {
        return CmdDiskMgrDelete(ArgC, ArgV);
    }
    
    if (StrCmp(SubCmd, "help") == 0 || StrCmp(SubCmd, "-h") == 0 || StrCmp(SubCmd, "--help") == 0) {
        CmdDiskMgrHelp();
        return 0;
    }
    
    BaseconPrintf(BASECON_TYPE_ERROR, "Unknown subcommand: %s\n", SubCmd);
    BaseconPrintf(BASECON_TYPE_NORMAL, "Try 'diskmgr help' for usage\n");
    return -1;
}

INT CmdDiskMgrRegister(VOID) {
    return CmdRegisterBuiltin("diskmgr", "Disk management utility", CmdDiskMgrHandler);
}