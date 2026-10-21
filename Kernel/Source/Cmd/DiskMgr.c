#include <Drivers/Disk/DiskMgr.h>
#include <Drivers/Disk/Pata.h>
#include <Mem/Allocator.h>
#include <Cmd/DiskMgr.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Fs/Vfs.h>
#include <Basecon.h>
#include <Return.h>

/* ============================================================================
 * Helpers
 * ============================================================================ */

STATIC VOID CmdFormatSize(UINT64 Bytes, CHAR *Out, USIZE OutSize) {
    if (Bytes >= 1024ULL * 1024 * 1024) {
        SnPrintf(Out, OutSize, "%.2f GB", (FLOAT)Bytes / (1024.0 * 1024 * 1024));
    } else if (Bytes >= 1024ULL * 1024) {
        SnPrintf(Out, OutSize, "%.2f MB", (FLOAT)Bytes / (1024.0 * 1024));
    } else if (Bytes >= 1024ULL) {
        SnPrintf(Out, OutSize, "%.2f KB", (FLOAT)Bytes / 1024.0);
    } else {
        SnPrintf(Out, OutSize, "%llu B", Bytes);
    }
}

/* Prints the GUID in the format XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX */
STATIC VOID CmdFormatGuid(const UINT8 Raw[16], CHAR *Out, USIZE OutSize) {
    UINT32 D1 = (UINT32)Raw[0]        | ((UINT32)Raw[1] << 8)
              | ((UINT32)Raw[2] << 16)| ((UINT32)Raw[3] << 24);
    UINT16 D2 = (UINT16)Raw[4]        | ((UINT16)Raw[5] << 8);
    UINT16 D3 = (UINT16)Raw[6]        | ((UINT16)Raw[7] << 8);

    SnPrintf(Out, OutSize,
             "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             D1, D2, D3,
             Raw[8], Raw[9],
             Raw[10], Raw[11], Raw[12], Raw[13], Raw[14], Raw[15]);
}

/* Checking if the volume is mounted (by the name of the disk or partition). */
STATIC BOOL CmdIsVolumeMounted(const CHAR *VolName) {
    if (!VolName || !VfsRootInode) return FALSE;

    CHAR CheckPath[80];
    SnPrintf(CheckPath, sizeof(CheckPath), "%s::/", VolName);

    VfsInode *CheckRoot = NULLPTR;
    if (VfsWalk(VfsRootInode, CheckPath, &CheckRoot) == 0) {
        VfsInodeUnref(CheckRoot);
        return TRUE;
    }
    return FALSE;
}

/* ============================================================================
 * Print: disk / partition
 * ============================================================================ */

STATIC VOID CmdPrintDiskInfo(Disk *D) {
    if (!D) return;

    CHAR SizeStr[32];
    CmdFormatSize(D->TotalSize, SizeStr, sizeof(SizeStr));

    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  %-10s %-7s %-6s %-12s %-10llu %s\n",
                  D->Name,
                  DiskTypeToString(D->Type),
                  DiskPartSchemeToString(D->PartScheme),
                  SizeStr,
                  D->SectorCount,
                  D->Initialized ? "OK" : "FAIL");

    /* DiskCfg-string */
    if (D->CfgValid || D->Alias[0] || D->IsRoot) {
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "    cfg: id=%s alias='%s' is_root=%u%s\n",
                      D->CfgId[0] ? D->CfgId : "-",
                      D->Alias[0] ? D->Alias : "",
                      D->IsRoot ? 1 : 0,
                      D->CfgStale ? " [STALE]" : "");
    }
}

STATIC VOID CmdPrintPartitionInfo(Partition *P) {
    if (!P) return;

    CHAR SizeStr[32];
    CmdFormatSize(P->SizeInBytes, SizeStr, sizeof(SizeStr));

    const CHAR *TypeStr = PartitionTypeToString(P->Type);

    if (P->IsGpt) {
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "  %-12s %-12s %-8s %-4s %s%s%s\n",
                      P->Name,
                      SizeStr,
                      TypeStr,
                      "gpt",
                      P->Label[0] ? "'" : "",
                      P->Label[0] ? P->Label : "",
                      P->Label[0] ? "'" : "");
    } else {
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "  %-12s %-12s %-8s 0x%02X %s\n",
                      P->Name,
                      SizeStr,
                      TypeStr,
                      P->PartitionId,
                      P->Bootable ? "BOOT" : "");
    }

    /* DiskCfg-string */
    if (P->CfgValid || P->Alias[0] || P->IsRoot) {
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "    cfg: id=%s alias='%s' is_root=%u%s\n",
                      P->CfgId[0] ? P->CfgId : "-",
                      P->Alias[0] ? P->Alias : "",
                      P->IsRoot ? 1 : 0,
                      P->CfgStale ? " [STALE]" : "");
    }
}

/* ============================================================================
 * Subcommand: list
 * ============================================================================ */

STATIC INT CmdDiskMgrList(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Available Disks:\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  NAME       TYPE    SCHEME SIZE         SECTORS    STATUS\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  ---------- ------- ------ ------------ ---------- ------\n");

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
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "Total: %u disk(s)\n", DiskMgrGetDiskCount());
    return 0;
}

/* ============================================================================
 * Subcommand: info
 * ============================================================================ */

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
    BaseconPrintf(BASECON_TYPE_NORMAL, "  Scheme:        %s\n",
                  DiskPartSchemeToString(D->PartScheme));
    BaseconPrintf(BASECON_TYPE_NORMAL, "  Total Size:    %llu bytes\n", D->TotalSize);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  Sectors:       %llu\n", D->SectorCount);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  Sector Size:   %u bytes\n", D->SectorSize);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  Partitions:    %u (primary), %u (logical)\n",
                  D->PartitionCount, D->LogicalCount);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  Initialized:   %s\n",
                  D->Initialized ? "Yes" : "No");

    if (D->PartScheme == DISK_PART_SCHEME_GPT) {
        CHAR GuidStr[40];
        CmdFormatGuid(D->DiskGuid, GuidStr, sizeof(GuidStr));
        BaseconPrintf(BASECON_TYPE_NORMAL, "  Disk GUID:     %s\n", GuidStr);
    }

    /* DiskCfg */
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n  === DiskCfg ===\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  id:            %s\n",
                  D->CfgId[0] ? D->CfgId : "(none)");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  alias:         %s\n",
                  D->Alias[0] ? D->Alias : "(none)");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  is_root:       %u\n", D->IsRoot ? 1 : 0);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  cfg_valid:     %u\n", D->CfgValid ? 1 : 0);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  cfg_stale:     %u\n", D->CfgStale ? 1 : 0);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  fs_kind:       %s\n",
                  D->CfgFsKind == DISKCFG_FS_FAT32 ? "FAT32" :
                  D->CfgFsKind == DISKCFG_FS_EXFAT ? "exFAT" :
                  D->CfgFsKind == DISKCFG_FS_ISO9660 ? "ISO9660" : "none");

    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    return 0;
}

/* ============================================================================
 * Subcommand: parts
 * ============================================================================ */

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
    BaseconPrintf(BASECON_TYPE_NORMAL, "=== Partitions on %s (%s) ===\n",
                  D->Name, DiskPartSchemeToString(D->PartScheme));

    if (D->PartScheme == DISK_PART_SCHEME_GPT) {
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "  NAME         SIZE         TYPE     KIND LABEL\n");
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "  ------------ ------------ -------- ---- -----\n");
    } else {
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "  NAME         SIZE         TYPE     ID   BOOT\n");
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "  ------------ ------------ -------- ---- ----\n");
    }

    for (UINT32 I = 0; I < D->PartitionCount; I++) {
        Partition *P = DiskMgrGetPartition(D, I);
        if (P) CmdPrintPartitionInfo(P);
    }
    for (UINT32 I = 0; I < D->LogicalCount; I++) {
        Partition *P = DiskMgrGetLogical(D, I);
        if (P) CmdPrintPartitionInfo(P);
    }

    if (D->PartitionCount == 0 && D->LogicalCount == 0) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "  No partitions found\n");
    }

    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "Total: %u partition(s)\n",
                  D->PartitionCount + D->LogicalCount);
    return 0;
}

/* ============================================================================
 * Helpers: section type analysis
 * ============================================================================ */

STATIC BOOL ParseMbrType(const CHAR *Name, UINT8 *Out) {
    if (!Name || !Out) return FALSE;

    if (StrCaseCmp(Name, "FAT32") == 0) { *Out = 0x0B; return TRUE; }
    if (StrCaseCmp(Name, "FAT16") == 0) { *Out = 0x06; return TRUE; }
    if (StrCaseCmp(Name, "NTFS") == 0)  { *Out = 0x07; return TRUE; }
    if (StrCaseCmp(Name, "Linux") == 0) { *Out = 0x83; return TRUE; }
    if (StrCaseCmp(Name, "EFI") == 0)   { *Out = 0xEF; return TRUE; }
    if (StrCaseCmp(Name, "SWAP") == 0)  { *Out = 0x82; return TRUE; }

    UINT8 Hex = AToHex(Name);
    if (Hex != 0) { *Out = Hex; return TRUE; }

    return FALSE;
}

STATIC BOOL ParseGptType(const CHAR *Name, PartitionType *Out) {
    if (!Name || !Out) return FALSE;

    if (StrCaseCmp(Name, "FAT32") == 0) { *Out = PARTITION_TYPE_FAT32;      return TRUE; }
    if (StrCaseCmp(Name, "FAT16") == 0) { *Out = PARTITION_TYPE_FAT16;      return TRUE; }
    if (StrCaseCmp(Name, "NTFS") == 0)  { *Out = PARTITION_TYPE_NTFS;       return TRUE; }
    if (StrCaseCmp(Name, "Linux") == 0) { *Out = PARTITION_TYPE_EXT4;       return TRUE; }
    if (StrCaseCmp(Name, "EXT4") == 0)  { *Out = PARTITION_TYPE_EXT4;       return TRUE; }
    if (StrCaseCmp(Name, "EXT3") == 0)  { *Out = PARTITION_TYPE_EXT3;       return TRUE; }
    if (StrCaseCmp(Name, "EXT2") == 0)  { *Out = PARTITION_TYPE_EXT2;       return TRUE; }
    if (StrCaseCmp(Name, "EFI") == 0)   { *Out = PARTITION_TYPE_EFI_SYSTEM; return TRUE; }
    if (StrCaseCmp(Name, "SWAP") == 0)  { *Out = PARTITION_TYPE_LINUX_SWAP; return TRUE; }

    return FALSE;
}

/* ============================================================================
 * Subcommand: init
 * ============================================================================ */

STATIC INT CmdDiskMgrInit(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 4) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Usage: diskmgr init <disk> <gpt|mbr>\n");
        return -1;
    }

    const CHAR *DiskName = ArgV[2];
    const CHAR *Scheme   = ArgV[3];

    Disk *D = DiskMgrFindDisk(DiskName);
    if (!D) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Disk not found: %s\n", DiskName);
        return -1;
    }

    if (!D->Write) {
        BaseconPrintf(BASECON_TYPE_ERROR, "%s is read-only\n", D->Name);
        return -1;
    }

    INT Ret;

    if (StrCaseCmp(Scheme, "gpt") == 0) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "Initializing GPT on %s...\n", D->Name);
        Ret = GptInitDisk(D);
        if (Ret != SUCCESS) {
            BaseconPrintf(BASECON_TYPE_ERROR, "GPT init failed (%d)\n", Ret);
            return -1;
        }
        BaseconPrintf(BASECON_TYPE_SUCCESS, "GPT initialized on %s\n", D->Name);
        return 0;
    }

    if (StrCaseCmp(Scheme, "mbr") == 0) {
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "MBR init is implicit: just 'create' a partition.\n");
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "Use: diskmgr create %s <size_mb> <type>\n", D->Name);
        return 0;
    }

    BaseconPrintf(BASECON_TYPE_ERROR, "Unknown scheme: %s (use gpt or mbr)\n", Scheme);
    return -1;
}

/* ============================================================================
 * Subcommand: create
 * ============================================================================ */

STATIC INT CmdDiskMgrCreate(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 5) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "Usage: diskmgr create <disk> <size_mb> <type> [flags]\n");
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "  MBR: <type> = FAT32 | NTFS | Linux | EFI | SWAP | FAT16 | 0xNN\n");
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "  GPT: add '-gpt' and optional '-label <name>'\n");
        return -1;
    }

    const CHAR *DiskName = ArgV[2];
    INT SizeMBInt = AToI(ArgV[3]);
    if (SizeMBInt <= 0) {
    	BaseconPrintf(BASECON_TYPE_ERROR, "Invalid size: %s\n", ArgV[3]);
    	return -1;
    }
    UINT64 SizeMB = (UINT64)SizeMBInt;
    const CHAR *TypeName = ArgV[4];

    Disk *D = DiskMgrFindDisk(DiskName);
    if (!D) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Disk not found: %s\n", DiskName);
        return -1;
    }

    BOOL UseGpt = FALSE;
    const CHAR *Label = NULLPTR;
    UINT64 ForcedStartLba = 0;
    BOOL HasStart = FALSE;

    for (INT I = 5; I < ArgC; I++) {
        if (StrCaseCmp(ArgV[I], "-gpt") == 0) {
            UseGpt = TRUE;
        } else if (StrCaseCmp(ArgV[I], "-label") == 0 && I + 1 < ArgC) {
            Label = ArgV[++I];
        } else if (StrCaseCmp(ArgV[I], "-start") == 0 && I + 1 < ArgC) {
            ForcedStartLba = (UINT64)AToI(ArgV[++I]);
            HasStart = TRUE;
        } else {
            if (D->PartScheme == DISK_PART_SCHEME_GPT) {
                UseGpt = TRUE;
            }
        }
    }

    if (D->PartScheme == DISK_PART_SCHEME_GPT) UseGpt = TRUE;

    UINT64 Sectors = (SizeMB * 1024 * 1024) / D->SectorSize;
    if (Sectors == 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Size too small\n");
        return -1;
    }

    if (UseGpt) {
        if (D->PartScheme != DISK_PART_SCHEME_GPT) {
            BaseconPrintf(BASECON_TYPE_ERROR,
                          "%s is not initialized as GPT. Run:\n", D->Name);
            BaseconPrintf(BASECON_TYPE_ERROR,
                          "  diskmgr init %s gpt\n", D->Name);
            return -1;
        }

        PartitionType GptType;
        if (!ParseGptType(TypeName, &GptType)) {
            BaseconPrintf(BASECON_TYPE_ERROR,
                          "Unknown GPT type: %s\n", TypeName);
            BaseconPrintf(BASECON_TYPE_NORMAL,
                          "Supported: FAT32 FAT16 NTFS Linux EXT4 EFI SWAP\n");
            return -1;
        }

        UINT64 StartLba = ForcedStartLba;
        if (!HasStart) {
            StartLba = 2048;
            for (UINT32 I = 0; I < D->PartitionCount; I++) {
                Partition *P = DiskMgrGetPartition(D, I);
                if (!P) continue;
                UINT64 End = P->StartLba + P->SectorCount;
                if (End > StartLba) StartLba = End;
            }
            StartLba = (StartLba + 2047) & ~(UINT64)2047;
        }

        if (StartLba + Sectors > D->SectorCount) {
            BaseconPrintf(BASECON_TYPE_ERROR, "Not enough space\n");
            return -1;
        }

        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "Creating GPT partition on %s: start=%llu, sectors=%llu, type=%s%s%s\n",
                      D->Name, StartLba, Sectors, TypeName,
                      Label ? ", label='" : "",
                      Label ? Label : "");

        INT Ret = PartitionCreateGPT(D, StartLba, Sectors, GptType, Label);
        if (Ret != SUCCESS) {
            BaseconPrintf(BASECON_TYPE_ERROR, "Failed to create GPT partition (%d)\n", Ret);
            return -1;
        }

        BaseconPrintf(BASECON_TYPE_SUCCESS, "GPT partition created\n");
        return 0;
    }

    UINT8 MbrType;
    if (!ParseMbrType(TypeName, &MbrType)) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Unknown MBR type: %s\n", TypeName);
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "Supported: FAT32 FAT16 NTFS Linux EFI SWAP | 0xNN\n");
        return -1;
    }

    UINT64 StartLba;
    if (HasStart) {
        StartLba = ForcedStartLba;
    } else {
        StartLba = D->SectorCount - Sectors;
        if (StartLba < 2048) StartLba = 2048;
    }

    if (StartLba + Sectors > D->SectorCount) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Not enough space\n");
        return -1;
    }

    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "Creating MBR partition on %s: start=%llu, sectors=%llu, type=%s (0x%02X)\n",
                  D->Name, StartLba, Sectors, TypeName, MbrType);

    INT Ret = PartitionCreateMBR(D, StartLba, Sectors, MbrType);
    if (Ret != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Failed to create MBR partition (%d)\n", Ret);
        return -1;
    }

    BaseconPrintf(BASECON_TYPE_SUCCESS, "MBR partition created\n");
    return 0;
}

/* ============================================================================
 * Subcommand: delete
 * ============================================================================ */

STATIC INT CmdDiskMgrDelete(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 4) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "Usage: diskmgr delete <disk> <partition_index>\n");
        return -1;
    }

    const CHAR *DiskName = ArgV[2];
    UINT32 PartIndex = (UINT32)AToI(ArgV[3]);

    Disk *D = DiskMgrFindDisk(DiskName);
    if (!D) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Disk not found: %s\n", DiskName);
        return -1;
    }

    if (PartIndex == 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Partition index must be >= 1\n");
        return -1;
    }

    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "Deleting partition %u from %s (%s)...\n",
                  PartIndex, D->Name,
                  DiskPartSchemeToString(D->PartScheme));

    INT Ret;

    if (D->PartScheme == DISK_PART_SCHEME_GPT) {
        Ret = PartitionDeleteGPT(D, PartIndex);
    } else {
        if (PartIndex > 4) {
            BaseconPrintf(BASECON_TYPE_ERROR,
                          "MBR supports only 1-4\n");
            return -1;
        }
        Ret = PartitionDeleteMBR(D, PartIndex);
    }

    if (Ret != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Failed to delete partition (%d)\n", Ret);
        return -1;
    }

    BaseconPrintf(BASECON_TYPE_SUCCESS, "Partition deleted\n");
    return 0;
}

/* ============================================================================
 * Subcommand: label
 * ============================================================================ */

STATIC INT CmdDiskMgrLabel(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 4) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "Usage: diskmgr label <disk>:<part> <label>\n");
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "  Example: diskmgr label NVME1p1 rootfs\n");
        return -1;
    }

    const CHAR *PartName = ArgV[2];
    const CHAR *Label    = ArgV[3];

    Partition *P = DiskMgrFindPartition(PartName);
    if (!P) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Partition not found: %s\n", PartName);
        return -1;
    }

    if (!P->IsGpt) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "Only GPT partitions support labels (this is MBR)\n");
        return -1;
    }

    INT Ret = PartitionCreateGPT(P->ParentDisk, P->StartLba, P->SectorCount,
                                  P->Type, Label);
    if (Ret != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Label update failed (%d)\n", Ret);
        return -1;
    }

    BaseconPrintf(BASECON_TYPE_SUCCESS,
                  "Label set: %s -> '%s'\n", PartName, Label);
    return 0;
}

/* ============================================================================
 * DiskCfg: helpers
 * ============================================================================ */

/* Find Disk or Partition by volume name. Returns:
 *   Kind: 0 — not found, 1 — Disk, 2 — Partition.
 * Fills in *OutDisk / *Output, StartLba, SectorCount, FsKind. */
STATIC INT CmdResolveVolume(const CHAR *Name,
                             Disk **OutDisk, Partition **OutPart) {
    if (!Name) return 0;

    /* 1. Disk by name */
    Disk *D = DiskMgrFindDisk(Name);
    if (D) {
        *OutDisk = D;
        *OutPart = NULLPTR;
        return 1;
    }

    /* 2. Partition by name (SATA0:1 / SATA0p1) */
    Partition *P = DiskMgrFindPartition(Name);
    if (P) {
        *OutDisk = P->ParentDisk;
        *OutPart = P;
        return 2;
    }

    /* 3. Disk/partition by alias */
    Disk *It = DiskMgrGetFirst();
    while (It) {
        if (It->Alias[0] && StrCaseCmp(It->Alias, Name) == 0) {
            *OutDisk = It;
            *OutPart = NULLPTR;
            return 1;
        }
        for (UINT32 I = 0; I < It->PartitionCount; I++) {
            Partition *Pp = DiskMgrGetPartition(It, I);
            if (Pp && Pp->Alias[0] && StrCaseCmp(Pp->Alias, Name) == 0) {
                *OutDisk = It;
                *OutPart = Pp;
                return 2;
            }
        }
        for (UINT32 I = 0; I < It->LogicalCount; I++) {
            Partition *Pp = DiskMgrGetLogical(It, I);
            if (Pp && Pp->Alias[0] && StrCaseCmp(Pp->Alias, Name) == 0) {
                *OutDisk = It;
                *OutPart = Pp;
                return 2;
            }
        }
        It = DiskMgrGetNext(It);
    }

    return 0;
}

/* Get start/sector/fs for volume. */
STATIC VOID CmdGetVolumeParams(Disk *D, Partition *P,
                                UINT64 *StartLba, UINT64 *SectorCount,
                                DiskCfgFsKind *Fs) {
    if (P) {
        *StartLba = P->StartLba;
        *SectorCount = P->SectorCount;
        *Fs = P->CfgFsKind;
    } else {
        *StartLba = 0;
        *SectorCount = D->SectorCount;
        *Fs = D->CfgFsKind;
    }
}

/* ============================================================================
 * Subcommand: cfg <volume>
 * ============================================================================ */

STATIC INT CmdDiskMgrCfg(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 3) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Usage: diskmgr cfg <volume>\n");
        return -1;
    }

    Disk *D = NULLPTR;
    Partition *P = NULLPTR;
    if (CmdResolveVolume(ArgV[2], &D, &P) == 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Volume not found: %s\n", ArgV[2]);
        return -1;
    }

    UINT64 StartLba, SectorCount;
    DiskCfgFsKind Fs;
    CmdGetVolumeParams(D, P, &StartLba, &SectorCount, &Fs);

    BaseconPrintf(BASECON_TYPE_NORMAL, "\n=== DiskCfg: %s ===\n",
                  P ? P->Name : D->Name);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  fs_kind:       %s\n",
                  Fs == DISKCFG_FS_FAT32 ? "FAT32" :
                  Fs == DISKCFG_FS_EXFAT ? "exFAT" :
                  Fs == DISKCFG_FS_ISO9660 ? "ISO9660" : "none");

    const CHAR *CfgId   = P ? P->CfgId   : D->CfgId;
    const CHAR *Alias   = P ? P->Alias   : D->Alias;
    BOOL CfgValid       = P ? P->CfgValid : D->CfgValid;
    BOOL CfgStale       = P ? P->CfgStale : D->CfgStale;
    BOOL IsRoot         = P ? P->IsRoot   : D->IsRoot;

    BaseconPrintf(BASECON_TYPE_NORMAL, "  id:            %s\n",
                  CfgId[0] ? CfgId : "(none)");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  alias:         %s\n",
                  Alias[0] ? Alias : "(none)");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  is_root:       %u\n", IsRoot ? 1 : 0);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  cfg_valid:     %u\n", CfgValid ? 1 : 0);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  cfg_stale:     %u\n", CfgStale ? 1 : 0);
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");

    return 0;
}

/* ============================================================================
 * Subcommand: set <volume> <key> <value>
 *   key: disk_name | is_root
 * ============================================================================ */

STATIC INT CmdDiskMgrSet(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 5) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "Usage: diskmgr set <volume> <key> <value>\n");
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "  keys: disk_name, is_root\n");
        return -1;
    }

    Disk *D = NULLPTR;
    Partition *P = NULLPTR;
    if (CmdResolveVolume(ArgV[2], &D, &P) == 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Volume not found: %s\n", ArgV[2]);
        return -1;
    }

    const CHAR *Key   = ArgV[3];
    const CHAR *Value = ArgV[4];

    if (!D->Write) {
        BaseconPrintf(BASECON_TYPE_ERROR, "%s is read-only\n", D->Name);
        return -1;
    }

    /* Determining whether the volume is mounted */
    const CHAR *VolName = P ? P->Name : D->Name;
    if (CmdIsVolumeMounted(VolName)) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "%s is mounted. Unmount first: unmount %s\n",
                      VolName, VolName);
        return -1;
    }

    UINT64 StartLba, SectorCount;
    DiskCfgFsKind Fs;
    CmdGetVolumeParams(D, P, &StartLba, &SectorCount, &Fs);

    if (Fs == DISKCFG_FS_NONE) {
        const CHAR *FsName = VfsDetectFilesystemAt(D, StartLba);
        if (FsName) {
            if (StrCmp(FsName, "fat32") == 0)      Fs = DISKCFG_FS_FAT32;
            else if (StrCmp(FsName, "exfat") == 0) Fs = DISKCFG_FS_EXFAT;
        }
        /* Caching the result */
        if (P) P->CfgFsKind = Fs;
        else   D->CfgFsKind = Fs;
    }

    if (Fs != DISKCFG_FS_FAT32 && Fs != DISKCFG_FS_EXFAT) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "Volume has no writable FS\n");
        return -1;
    }

    /* Current values */
    const CHAR *CurName = P ? P->Alias : D->Alias;
    BOOL CurIsRoot = P ? P->IsRoot : D->IsRoot;

    CHAR NewName[DISKCFG_NAME_MAX];
    StrnCpy(NewName, CurName, sizeof(NewName) - 1);
    BOOL NewIsRoot = CurIsRoot;

    if (StrCaseCmp(Key, "disk_name") == 0) {
        StrnCpy(NewName, Value, sizeof(NewName) - 1);
    } else if (StrCaseCmp(Key, "is_root") == 0) {
        NewIsRoot = (AToI(Value) != 0);
    } else {
        BaseconPrintf(BASECON_TYPE_ERROR, "Unknown key: %s\n", Key);
        return -1;
    }

    INT Rc = DiskCfgWriteVolume(D, StartLba, SectorCount, Fs,
                                 NewName, NewIsRoot);
    if (Rc != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Write failed (%d)\n", Rc);
        return -1;
    }

    /* Refresh the cache in memory */
    if (P) {
        StrnCpy(P->Alias, NewName, sizeof(P->Alias) - 1);
        P->IsRoot = NewIsRoot;
        P->CfgValid = TRUE;
    } else {
        StrnCpy(D->Alias, NewName, sizeof(D->Alias) - 1);
        D->IsRoot = NewIsRoot;
        D->CfgValid = TRUE;
    }

    BaseconPrintf(BASECON_TYPE_SUCCESS,
                  "Updated %s: %s=%s\n", VolName, Key, Value);
    return 0;
}

/* ============================================================================
 * Subcommand: clear <volume>
 * ============================================================================ */

STATIC INT CmdDiskMgrClear(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 3) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Usage: diskmgr clear <volume>\n");
        return -1;
    }

    Disk *D = NULLPTR;
    Partition *P = NULLPTR;
    if (CmdResolveVolume(ArgV[2], &D, &P) == 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Volume not found: %s\n", ArgV[2]);
        return -1;
    }

    UINT64 StartLba, SectorCount;
    DiskCfgFsKind Fs;
    CmdGetVolumeParams(D, P, &StartLba, &SectorCount, &Fs);

    if (Fs != DISKCFG_FS_FAT32 && Fs != DISKCFG_FS_EXFAT) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "Volume has no writable FS (fs_kind=%d)\n", (INT)Fs);
        return -1;
    }

    const CHAR *VolName = P ? P->Name : D->Name;
    if (CmdIsVolumeMounted(VolName)) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "%s is mounted. Unmount first.\n", VolName);
        return -1;
    }

    /* Writing an empty disk.cfg is actually a "reset" */
    INT Rc = DiskCfgWriteVolume(D, StartLba, SectorCount, Fs, "", FALSE);
    if (Rc != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR, "Write failed (%d)\n", Rc);
        return -1;
    }

    if (P) {
        P->Alias[0] = '\0';
        P->IsRoot = FALSE;
        P->CfgValid = FALSE;
        P->CfgStale = FALSE;
    } else {
        D->Alias[0] = '\0';
        D->IsRoot = FALSE;
        D->CfgValid = FALSE;
        D->CfgStale = FALSE;
    }

    BaseconPrintf(BASECON_TYPE_SUCCESS, "Cleared cfg on %s\n", VolName);
    return 0;
}

/* ============================================================================
 * Subcommand: rename <volume> <new_name>
 * ============================================================================ */

STATIC INT CmdDiskMgrRename(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 4) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "Usage: diskmgr rename <volume> <new_name>\n");
        return -1;
    }

    /* Proxy for set disk_names */
    const CHAR *NewArgV[5];
    NewArgV[0] = ArgV[0];
    NewArgV[1] = "set";
    NewArgV[2] = ArgV[2];
    NewArgV[3] = "disk_name";
    NewArgV[4] = ArgV[3];

    return CmdDiskMgrSet(5, NewArgV);
}

/* ============================================================================
 * Subcommand: setroot / unsetroot
 * ============================================================================ */

STATIC INT CmdDiskMgrSetRoot(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 3) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "Usage: diskmgr setroot <volume>\n");
        return -1;
    }

    const CHAR *NewArgV[5];
    NewArgV[0] = ArgV[0];
    NewArgV[1] = "set";
    NewArgV[2] = ArgV[2];
    NewArgV[3] = "is_root";
    NewArgV[4] = "1";
    return CmdDiskMgrSet(5, NewArgV);
}

STATIC INT CmdDiskMgrUnsetRoot(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 3) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "Usage: diskmgr unsetroot <volume>\n");
        return -1;
    }

    const CHAR *NewArgV[5];
    NewArgV[0] = ArgV[0];
    NewArgV[1] = "set";
    NewArgV[2] = ArgV[2];
    NewArgV[3] = "is_root";
    NewArgV[4] = "0";
    return CmdDiskMgrSet(5, NewArgV);
}

/* ============================================================================
 * Help
 * ============================================================================ */

STATIC VOID CmdDiskMgrHelp(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "=== DiskMgr Commands ===\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  diskmgr list                        List all disks\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  diskmgr info <disk>                 Show disk information\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  diskmgr parts <disk>                List partitions on disk\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  diskmgr init <disk> gpt             Initialize GPT\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  diskmgr create <disk> <size> <type> [flags]\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "      MBR:  diskmgr create SATA0 100 FAT32\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "      GPT:  diskmgr create NVME1 100 ext4 -gpt -label rootfs\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "      Optional: -start <lba> (manual placement)\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  diskmgr delete <disk> <idx>         Delete partition\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  diskmgr label <disk>p<idx> <label>  Set GPT label\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  === DiskCfg ===\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  diskmgr cfg <volume>                Show disk.cfg for volume\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  diskmgr set <volume> <key> <value>  Change disk.cfg\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "      keys: disk_name, is_root\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  diskmgr clear <volume>              Reset disk.cfg\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  diskmgr rename <volume> <new_name>  Shortcut for set disk_name\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  diskmgr setroot <volume>            Shortcut for set is_root=1\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  diskmgr unsetroot <volume>          Shortcut for set is_root=0\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  diskmgr help                        Show this help\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Partition types:\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  NTFS FAT32 FAT16 Linux EXT4 EFI SWAP\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Schemes:\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  MBR  — legacy, max 4 primary, 2 TB max\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "  GPT  — modern, 128 partitions, > 2 TB, labels, GUIDs\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
}

/* ============================================================================
 * Handler
 * ============================================================================ */

INT CmdDiskMgrHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        CmdDiskMgrHelp();
        return 0;
    }

    CHAR SubCmd[32];
    StrnCpy(SubCmd, ArgV[1], sizeof(SubCmd) - 1);
    SubCmd[sizeof(SubCmd) - 1] = '\0';

    USIZE Len = StrLen(SubCmd);
    while (Len > 0 &&
           (SubCmd[Len-1] == '\r' || SubCmd[Len-1] == '\n' || SubCmd[Len-1] == ' ')) {
        SubCmd[--Len] = '\0';
    }

    if (StrCmp(SubCmd, "list") == 0 || StrCmp(SubCmd, "ls") == 0) {
        return CmdDiskMgrList();
    }
    if (StrCmp(SubCmd, "info") == 0) {
        return CmdDiskMgrInfo(ArgC >= 3 ? ArgV[2] : NULLPTR);
    }
    if (StrCmp(SubCmd, "parts") == 0 || StrCmp(SubCmd, "partitions") == 0) {
        return CmdDiskMgrPartitions(ArgC >= 3 ? ArgV[2] : NULLPTR);
    }
    if (StrCmp(SubCmd, "init") == 0) {
        return CmdDiskMgrInit(ArgC, ArgV);
    }
    if (StrCmp(SubCmd, "create") == 0 || StrCmp(SubCmd, "add") == 0) {
        return CmdDiskMgrCreate(ArgC, ArgV);
    }
    if (StrCmp(SubCmd, "delete") == 0 || StrCmp(SubCmd, "del") == 0) {
        return CmdDiskMgrDelete(ArgC, ArgV);
    }
    if (StrCmp(SubCmd, "label") == 0) {
        return CmdDiskMgrLabel(ArgC, ArgV);
    }
    if (StrCmp(SubCmd, "cfg") == 0) {
        return CmdDiskMgrCfg(ArgC, ArgV);
    }
    if (StrCmp(SubCmd, "set") == 0) {
        return CmdDiskMgrSet(ArgC, ArgV);
    }
    if (StrCmp(SubCmd, "clear") == 0) {
        return CmdDiskMgrClear(ArgC, ArgV);
    }
    if (StrCmp(SubCmd, "rename") == 0) {
        return CmdDiskMgrRename(ArgC, ArgV);
    }
    if (StrCmp(SubCmd, "setroot") == 0) {
        return CmdDiskMgrSetRoot(ArgC, ArgV);
    }
    if (StrCmp(SubCmd, "unsetroot") == 0) {
        return CmdDiskMgrUnsetRoot(ArgC, ArgV);
    }
    if (StrCmp(SubCmd, "help") == 0 ||
        StrCmp(SubCmd, "-h") == 0 ||
        StrCmp(SubCmd, "--help") == 0) {
        CmdDiskMgrHelp();
        return 0;
    }

    BaseconPrintf(BASECON_TYPE_ERROR, "Unknown subcommand: %s\n", SubCmd);
    BaseconPrintf(BASECON_TYPE_NORMAL, "Try 'diskmgr help' for usage\n");
    return -1;
}

INT CmdDiskMgrRegister(VOID) {
    return CmdRegisterBuiltin("diskmgr",
                              "Disk management utility (MBR + GPT)",
                              CmdDiskMgrHandler);
}