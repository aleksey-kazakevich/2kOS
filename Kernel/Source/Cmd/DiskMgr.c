#include <Drivers/Disk/DiskMgr.h>
#include <Drivers/Disk/IsoFile.h>
#include <Limine/LimineParse.h>
#include <Drivers/Disk/Pata.h>
#include <Mem/Allocator.h>
#include <Cmd/DiskMgr.h>
#include <Fs/Iso9660.h>
#include <Lib/String.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <Lib/StdIo.h>
#include <Fs/Vfs.h>
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

    StdOutPrintf("  %-10s %-7s %-6s %-12s %-10llu %s\n",
                  D->Name,
                  DiskTypeToString(D->Type),
                  DiskPartSchemeToString(D->PartScheme),
                  SizeStr,
                  D->SectorCount,
                  D->Initialized ? "OK" : "FAIL");

    /* DiskCfg-string */
    if (D->CfgValid || D->Alias[0] || D->IsRoot) {
        StdOutPrintf("    cfg: id=%s alias='%s' is_root=%u%s\n",
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
        StdOutPrintf("  %-12s %-12s %-8s %-4s %s%s%s\n",
                      P->Name,
                      SizeStr,
                      TypeStr,
                      "gpt",
                      P->Label[0] ? "'" : "",
                      P->Label[0] ? P->Label : "",
                      P->Label[0] ? "'" : "");
    } else {
        StdOutPrintf("  %-12s %-12s %-8s 0x%02X %s\n",
                      P->Name,
                      SizeStr,
                      TypeStr,
                      P->PartitionId,
                      P->Bootable ? "BOOT" : "");
    }

    /* DiskCfg-string */
    if (P->CfgValid || P->Alias[0] || P->IsRoot) {
        StdOutPrintf("    cfg: id=%s alias='%s' is_root=%u%s\n",
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
    StdOutPrintf("\n");
    StdOutPrintf("Available Disks:\n");
    StdOutPrintf("  NAME       TYPE    SCHEME SIZE         SECTORS    STATUS\n");
    StdOutPrintf("  ---------- ------- ------ ------------ ---------- ------\n");

    Disk *D = DiskMgrGetFirst();
    if (!D) {
        StdOutPrintf("  No disks found\n");
        return 0;
    }

    while (D) {
        CmdPrintDiskInfo(D);
        D = DiskMgrGetNext(D);
    }

    StdOutPrintf("\n");
    StdOutPrintf("Total: %u disk(s)\n", DiskMgrGetDiskCount());
    return 0;
}

/* ============================================================================
 * Subcommand: info
 * ============================================================================ */

STATIC INT CmdDiskMgrInfo(const CHAR *DiskName) {
    if (!DiskName) {
        StdErrPrintf("Usage: diskmgr info <disk>\n");
        return -1;
    }

    Disk *D = DiskMgrFindDisk(DiskName);
    if (!D) {
        StdErrPrintf("Disk not found: %s\n", DiskName);
        return -1;
    }

    StdOutPrintf("\n");
    StdOutPrintf("=== Disk Information: %s ===\n", D->Name);
    StdOutPrintf("  Type:          %s\n", DiskTypeToString(D->Type));
    StdOutPrintf("  Scheme:        %s\n",
                  DiskPartSchemeToString(D->PartScheme));
    StdOutPrintf("  Total Size:    %llu bytes\n", D->TotalSize);
    StdOutPrintf("  Sectors:       %llu\n", D->SectorCount);
    StdOutPrintf("  Sector Size:   %u bytes\n", D->SectorSize);
    StdOutPrintf("  Partitions:    %u (primary), %u (logical)\n",
                  D->PartitionCount, D->LogicalCount);
    StdOutPrintf("  Initialized:   %s\n",
                  D->Initialized ? "Yes" : "No");

    if (D->PartScheme == DISK_PART_SCHEME_GPT) {
        CHAR GuidStr[40];
        CmdFormatGuid(D->DiskGuid, GuidStr, sizeof(GuidStr));
        StdOutPrintf("  Disk GUID:     %s\n", GuidStr);
    }

    /* DiskCfg */
    StdOutPrintf("\n  === DiskCfg ===\n");
    StdOutPrintf("  id:            %s\n",
                  D->CfgId[0] ? D->CfgId : "(none)");
    StdOutPrintf("  alias:         %s\n",
                  D->Alias[0] ? D->Alias : "(none)");
    StdOutPrintf("  is_root:       %u\n", D->IsRoot ? 1 : 0);
    StdOutPrintf("  cfg_valid:     %u\n", D->CfgValid ? 1 : 0);
    StdOutPrintf("  cfg_stale:     %u\n", D->CfgStale ? 1 : 0);
    StdOutPrintf("  fs_kind:       %s\n",
                  D->CfgFsKind == DISKCFG_FS_FAT32 ? "FAT32" :
                  D->CfgFsKind == DISKCFG_FS_EXFAT ? "exFAT" :
                  D->CfgFsKind == DISKCFG_FS_ISO9660 ? "ISO9660" : "none");

    StdOutPrintf("\n");
    return 0;
}

/* ============================================================================
 * Subcommand: parts
 * ============================================================================ */

STATIC INT CmdDiskMgrPartitions(const CHAR *DiskName) {
    if (!DiskName) {
        StdErrPrintf("Usage: diskmgr parts <disk>\n");
        return -1;
    }

    Disk *D = DiskMgrFindDisk(DiskName);
    if (!D) {
        StdErrPrintf("Disk not found: %s\n", DiskName);
        return -1;
    }

    StdOutPrintf("\n");
    StdOutPrintf("=== Partitions on %s (%s) ===\n",
                  D->Name, DiskPartSchemeToString(D->PartScheme));

    if (D->PartScheme == DISK_PART_SCHEME_GPT) {
        StdOutPrintf("  NAME         SIZE         TYPE     KIND LABEL\n");
        StdOutPrintf("  ------------ ------------ -------- ---- -----\n");
    } else {
        StdOutPrintf("  NAME         SIZE         TYPE     ID   BOOT\n");
        StdOutPrintf("  ------------ ------------ -------- ---- ----\n");
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
        StdOutPrintf("  No partitions found\n");
    }

    StdOutPrintf("\n");
    StdOutPrintf("Total: %u partition(s)\n",
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
        StdErrPrintf("Usage: diskmgr init <disk> <gpt|mbr>\n");
        return -1;
    }

    const CHAR *DiskName = ArgV[2];
    const CHAR *Scheme   = ArgV[3];

    Disk *D = DiskMgrFindDisk(DiskName);
    if (!D) {
        StdErrPrintf("Disk not found: %s\n", DiskName);
        return -1;
    }

    if (!D->Write) {
        StdErrPrintf("%s is read-only\n", D->Name);
        return -1;
    }

    INT Ret;

    if (StrCaseCmp(Scheme, "gpt") == 0) {
        StdOutPrintf("Initializing GPT on %s...\n", D->Name);
        Ret = GptInitDisk(D);
        if (Ret != SUCCESS) {
            StdErrPrintf("GPT init failed (%d)\n", Ret);
            return -1;
        }
        StdOutPrintf("GPT initialized on %s\n", D->Name);
        return 0;
    }

    if (StrCaseCmp(Scheme, "mbr") == 0) {
        StdOutPrintf("MBR init is implicit: just 'create' a partition.\n");
        StdOutPrintf("Use: diskmgr create %s <size_mb> <type>\n", D->Name);
        return 0;
    }

    StdErrPrintf("Unknown scheme: %s (use gpt or mbr)\n", Scheme);
    return -1;
}

/* ============================================================================
 * Subcommand: create
 * ============================================================================ */

STATIC INT CmdDiskMgrCreate(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 5) {
        StdErrPrintf("Usage: diskmgr create <disk> <size_mb> <type> [flags]\n");
        StdOutPrintf("  MBR: <type> = FAT32 | NTFS | Linux | EFI | SWAP | FAT16 | 0xNN\n");
        StdOutPrintf("  GPT: add '-gpt' and optional '-label <name>'\n");
        return -1;
    }

    const CHAR *DiskName = ArgV[2];
    INT SizeMBInt = AToI(ArgV[3]);
    if (SizeMBInt <= 0) {
    	StdErrPrintf("Invalid size: %s\n", ArgV[3]);
    	return -1;
    }
    UINT64 SizeMB = (UINT64)SizeMBInt;
    const CHAR *TypeName = ArgV[4];

    Disk *D = DiskMgrFindDisk(DiskName);
    if (!D) {
        StdErrPrintf("Disk not found: %s\n", DiskName);
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
        StdErrPrintf("Size too small\n");
        return -1;
    }

    if (UseGpt) {
        if (D->PartScheme != DISK_PART_SCHEME_GPT) {
            StdErrPrintf("%s is not initialized as GPT. Run:\n", D->Name);
            StdErrPrintf("  diskmgr init %s gpt\n", D->Name);
            return -1;
        }

        PartitionType GptType;
        if (!ParseGptType(TypeName, &GptType)) {
            StdErrPrintf("Unknown GPT type: %s\n", TypeName);
            StdOutPrintf("Supported: FAT32 FAT16 NTFS Linux EXT4 EFI SWAP\n");
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
            StdErrPrintf("Not enough space\n");
            return -1;
        }

        StdOutPrintf("Creating GPT partition on %s: start=%llu, sectors=%llu, type=%s%s%s\n",
                      D->Name, StartLba, Sectors, TypeName,
                      Label ? ", label='" : "",
                      Label ? Label : "");

        INT Ret = PartitionCreateGPT(D, StartLba, Sectors, GptType, Label);
        if (Ret != SUCCESS) {
            StdErrPrintf("Failed to create GPT partition (%d)\n", Ret);
            return -1;
        }

        StdOutPrintf("GPT partition created\n");
        return 0;
    }

    UINT8 MbrType;
    if (!ParseMbrType(TypeName, &MbrType)) {
        StdErrPrintf("Unknown MBR type: %s\n", TypeName);
        StdOutPrintf("Supported: FAT32 FAT16 NTFS Linux EFI SWAP | 0xNN\n");
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
        StdErrPrintf("Not enough space\n");
        return -1;
    }

    StdOutPrintf("Creating MBR partition on %s: start=%llu, sectors=%llu, type=%s (0x%02X)\n",
                  D->Name, StartLba, Sectors, TypeName, MbrType);

    INT Ret = PartitionCreateMBR(D, StartLba, Sectors, MbrType);
    if (Ret != SUCCESS) {
        StdErrPrintf("Failed to create MBR partition (%d)\n", Ret);
        return -1;
    }

    StdOutPrintf("MBR partition created\n");
    return 0;
}

/* ============================================================================
 * Subcommand: delete
 * ============================================================================ */

STATIC INT CmdDiskMgrDelete(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 4) {
        StdErrPrintf("Usage: diskmgr delete <disk> <partition_index>\n");
        return -1;
    }

    const CHAR *DiskName = ArgV[2];
    UINT32 PartIndex = (UINT32)AToI(ArgV[3]);

    Disk *D = DiskMgrFindDisk(DiskName);
    if (!D) {
        StdErrPrintf("Disk not found: %s\n", DiskName);
        return -1;
    }

    if (PartIndex == 0) {
        StdErrPrintf("Partition index must be >= 1\n");
        return -1;
    }

    StdOutPrintf("Deleting partition %u from %s (%s)...\n",
                  PartIndex, D->Name,
                  DiskPartSchemeToString(D->PartScheme));

    INT Ret;

    if (D->PartScheme == DISK_PART_SCHEME_GPT) {
        Ret = PartitionDeleteGPT(D, PartIndex);
    } else {
        if (PartIndex > 4) {
            StdErrPrintf("MBR supports only 1-4\n");
            return -1;
        }
        Ret = PartitionDeleteMBR(D, PartIndex);
    }

    if (Ret != SUCCESS) {
        StdErrPrintf("Failed to delete partition (%d)\n", Ret);
        return -1;
    }

    StdOutPrintf("Partition deleted\n");
    return 0;
}

/* ============================================================================
 * Subcommand: label
 * ============================================================================ */

STATIC INT CmdDiskMgrLabel(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 4) {
        StdErrPrintf("Usage: diskmgr label <disk>:<part> <label>\n");
        StdOutPrintf("  Example: diskmgr label NVME1p1 rootfs\n");
        return -1;
    }

    const CHAR *PartName = ArgV[2];
    const CHAR *Label    = ArgV[3];

    Partition *P = DiskMgrFindPartition(PartName);
    if (!P) {
        StdErrPrintf("Partition not found: %s\n", PartName);
        return -1;
    }

    if (!P->IsGpt) {
        StdErrPrintf("Only GPT partitions support labels (this is MBR)\n");
        return -1;
    }

    INT Ret = PartitionCreateGPT(P->ParentDisk, P->StartLba, P->SectorCount,
                                  P->Type, Label);
    if (Ret != SUCCESS) {
        StdErrPrintf("Label update failed (%d)\n", Ret);
        return -1;
    }

    StdOutPrintf("Label set: %s -> '%s'\n", PartName, Label);
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
        StdErrPrintf("Usage: diskmgr cfg <volume>\n");
        return -1;
    }

    Disk *D = NULLPTR;
    Partition *P = NULLPTR;
    if (CmdResolveVolume(ArgV[2], &D, &P) == 0) {
        StdErrPrintf("Volume not found: %s\n", ArgV[2]);
        return -1;
    }

    UINT64 StartLba, SectorCount;
    DiskCfgFsKind Fs;
    CmdGetVolumeParams(D, P, &StartLba, &SectorCount, &Fs);

    StdOutPrintf("\n=== DiskCfg: %s ===\n",
                  P ? P->Name : D->Name);
    StdOutPrintf("  fs_kind:       %s\n",
                  Fs == DISKCFG_FS_FAT32 ? "FAT32" :
                  Fs == DISKCFG_FS_EXFAT ? "exFAT" :
                  Fs == DISKCFG_FS_ISO9660 ? "ISO9660" : "none");

    const CHAR *CfgId   = P ? P->CfgId   : D->CfgId;
    const CHAR *Alias   = P ? P->Alias   : D->Alias;
    BOOL CfgValid       = P ? P->CfgValid : D->CfgValid;
    BOOL CfgStale       = P ? P->CfgStale : D->CfgStale;
    BOOL IsRoot         = P ? P->IsRoot   : D->IsRoot;

    StdOutPrintf("  id:            %s\n",
                  CfgId[0] ? CfgId : "(none)");
    StdOutPrintf("  alias:         %s\n",
                  Alias[0] ? Alias : "(none)");
    StdOutPrintf("  is_root:       %u\n", IsRoot ? 1 : 0);
    StdOutPrintf("  cfg_valid:     %u\n", CfgValid ? 1 : 0);
    StdOutPrintf("  cfg_stale:     %u\n", CfgStale ? 1 : 0);
    StdOutPrintf("\n");

    return 0;
}

/* ============================================================================
 * Subcommand: set <volume> <key> <value>
 *   key: disk_name | is_root
 * ============================================================================ */

STATIC INT CmdDiskMgrSet(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 5) {
        StdErrPrintf("Usage: diskmgr set <volume> <key> <value>\n");
        StdOutPrintf("  keys: disk_name, is_root\n");
        return -1;
    }

    Disk *D = NULLPTR;
    Partition *P = NULLPTR;
    if (CmdResolveVolume(ArgV[2], &D, &P) == 0) {
        StdErrPrintf("Volume not found: %s\n", ArgV[2]);
        return -1;
    }

    const CHAR *Key   = ArgV[3];
    const CHAR *Value = ArgV[4];

    if (!D->Write) {
        StdErrPrintf("%s is read-only\n", D->Name);
        return -1;
    }

    /* Determining whether the volume is mounted */
    const CHAR *VolName = P ? P->Name : D->Name;
    if (CmdIsVolumeMounted(VolName)) {
        StdErrPrintf("%s is mounted. Unmount first: unmount %s\n",
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
        StdErrPrintf("Volume has no writable FS\n");
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
        StdErrPrintf("Unknown key: %s\n", Key);
        return -1;
    }

    INT Rc = DiskCfgWriteVolume(D, StartLba, SectorCount, Fs,
                                 NewName, NewIsRoot);
    if (Rc != SUCCESS) {
        StdErrPrintf("Write failed (%d)\n", Rc);
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

    StdOutPrintf("Updated %s: %s=%s\n", VolName, Key, Value);
    return 0;
}

/* ============================================================================
 * Subcommand: clear <volume>
 * ============================================================================ */

STATIC INT CmdDiskMgrClear(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 3) {
        StdErrPrintf("Usage: diskmgr clear <volume>\n");
        return -1;
    }

    Disk *D = NULLPTR;
    Partition *P = NULLPTR;
    if (CmdResolveVolume(ArgV[2], &D, &P) == 0) {
        StdErrPrintf("Volume not found: %s\n", ArgV[2]);
        return -1;
    }

    UINT64 StartLba, SectorCount;
    DiskCfgFsKind Fs;
    CmdGetVolumeParams(D, P, &StartLba, &SectorCount, &Fs);

    if (Fs != DISKCFG_FS_FAT32 && Fs != DISKCFG_FS_EXFAT) {
        StdErrPrintf("Volume has no writable FS (fs_kind=%d)\n", (INT)Fs);
        return -1;
    }

    const CHAR *VolName = P ? P->Name : D->Name;
    if (CmdIsVolumeMounted(VolName)) {
        StdErrPrintf("%s is mounted. Unmount first.\n", VolName);
        return -1;
    }

    /* Writing an empty disk.cfg is actually a "reset" */
    INT Rc = DiskCfgWriteVolume(D, StartLba, SectorCount, Fs, "", FALSE);
    if (Rc != SUCCESS) {
        StdErrPrintf("Write failed (%d)\n", Rc);
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

    StdOutPrintf("Cleared cfg on %s\n", VolName);
    return 0;
}

/* ============================================================================
 * Subcommand: rename <volume> <new_name>
 * ============================================================================ */

STATIC INT CmdDiskMgrRename(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 4) {
        StdErrPrintf("Usage: diskmgr rename <volume> <new_name>\n");
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
        StdErrPrintf("Usage: diskmgr setroot <volume>\n");
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
        StdErrPrintf("Usage: diskmgr unsetroot <volume>\n");
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

#define ROOTSETUP_MAX_PATH 512

STATIC const CHAR GRootSetupEnvConf[] =
    "# 2kOS environment\n"
    "#\n"
    "# BIN      — default directory for user-space binaries\n"
    "# PATH     — colon-separated list of directories searched for commands\n"
    "\n"
    "BIN=/bin\n"
    "PATH=/bin\n";

/* Look up the USER.iso Limine module. */
STATIC struct limine_file *RootSetupFindUserIso(VOID) {
    STATIC const CHAR *Candidates[] = {
        "USER.iso", "user.iso",
        "installfiles/USER.iso", "installfiles/user.iso",
        NULLPTR
    };

    for (INT I = 0; Candidates[I]; I++) {
        struct limine_file *F = LimineFindModuleByString(Candidates[I]);
        if (F) return F;
        F = LimineFindModuleByPath(Candidates[I]);
        if (F) return F;
    }
    return NULLPTR;
}

/* Ensure <Parent>/<Name> exists as a directory.
 * On SUCCESS returns +1 reference in *Out. */
STATIC INT RootSetupEnsureDir(VfsInode *Parent, const CHAR *Name,
                              VfsInode **Out) {
    if (!Parent || !Name || !Out) return INCORRECT_VALUE;

    VfsInode *Existing = NULLPTR;
    if (VfsWalk(Parent, Name, &Existing) == SUCCESS) {
        if (Existing->IMode != FT_DIR) {
            VfsInodeUnref(Existing);
            return INCORRECT_VALUE;
        }
        *Out = Existing;
        return SUCCESS;
    }

    return VfsMkdir(Parent, Name, FT_DIR, Out);
}

/* Write <Data>/<Size> to <Dir>/<Path>. */
STATIC INT RootSetupWriteFile(VfsInode *Dir, const CHAR *Path,
                              const VOID *Data, UINT32 Size) {
    VfsFile *F = NULLPTR;
    INT Rc = VfsOpen(Dir, Path,
                     VFS_O_WRITE | VFS_O_CREAT | VFS_O_TRUNC, &F);
    if (Rc != SUCCESS) {
        StdErrPrintf("rootsetup: VfsOpen('%s') failed (%d)\n", Path, Rc);
        return Rc;
    }

    UINT32 Written = 0;
    Rc = VfsWrite(F, Data, Size, &Written);
    VfsClose(F);

    if (Rc != SUCCESS || Written != Size) {
        StdErrPrintf("rootsetup: VfsWrite('%s') failed (%d, %u/%u)\n",
                     Path, Rc, Written, Size);
        return IO_ERROR;
    }
    return SUCCESS;
}

/* Recursively copy <SrcDir> into <DstDir>. <RelPrefix> is used only for
 * error messages (e.g. "bin/subdir"). */
STATIC INT RootSetupCopyTree(VfsInode *SrcDir, VfsInode *DstDir,
                             const CHAR *RelPrefix, UINT32 *Copied) {
    if (!SrcDir || !DstDir) return INCORRECT_VALUE;

    UINT64 Pos = 0;
    CHAR Name[NAME_MAX + 1];
    UINT32 NameLen = 0;
    UINT32 Type = 0;

    while (VfsReadDir(SrcDir, &Pos, Name, &NameLen, &Type) == SUCCESS) {
        if (NameLen == 0) continue;
        if ((NameLen == 1 && Name[0] == '.') ||
            (NameLen == 2 && Name[0] == '.' && Name[1] == '.'))
            continue;

        Name[NameLen] = '\0';

        CHAR RelPath[ROOTSETUP_MAX_PATH];
        if (RelPrefix && RelPrefix[0]) {
            SnPrintf(RelPath, sizeof(RelPath), "%s/%s", RelPrefix, Name);
        } else {
            SnPrintf(RelPath, sizeof(RelPath), "%s", Name);
        }

        if (Type == FT_DIR) {
            VfsInode *SubDst = NULLPTR;
            INT Rc = RootSetupEnsureDir(DstDir, Name, &SubDst);
            if (Rc != SUCCESS) {
                StdErrPrintf("rootsetup: mkdir '%s' failed (%d)\n",
                             RelPath, Rc);
                return Rc;
            }

            VfsInode *SubSrc = NULLPTR;
            Rc = VfsWalk(SrcDir, Name, &SubSrc);
            if (Rc != SUCCESS) {
                StdErrPrintf("rootsetup: walk '%s' failed (%d)\n",
                             RelPath, Rc);
                VfsInodeUnref(SubDst);
                return Rc;
            }

            Rc = RootSetupCopyTree(SubSrc, SubDst, RelPath, Copied);
            VfsInodeUnref(SubSrc);
            VfsInodeUnref(SubDst);
            if (Rc != SUCCESS) return Rc;

        } else if (Type == FT_REG_FILE) {
            VfsFile *Src = NULLPTR;
            INT Rc = VfsOpen(SrcDir, Name, VFS_O_READ, &Src);
            if (Rc != SUCCESS) {
                StdErrPrintf("rootsetup: open '%s' failed (%d)\n",
                             RelPath, Rc);
                return Rc;
            }

            UINT64 Size = Src->FInode ? Src->FInode->ISize : 0;
            if (Size == 0) {
                VfsClose(Src);
                Rc = RootSetupWriteFile(DstDir, Name, NULLPTR, 0);
                if (Rc == SUCCESS) (*Copied)++;
                continue;
            }
            if (Size > 64 * 1024 * 1024) {
                VfsClose(Src);
                StdErrPrintf("rootsetup: '%s' too large (%llu)\n",
                             RelPath, Size);
                return NO_MEMORY;
            }

            UINT8 *Buf = (UINT8 *)MemoryAllocate((USIZE)Size);
            if (!Buf) {
                VfsClose(Src);
                return NO_MEMORY;
            }

            UINT32 Got = 0;
            Rc = VfsRead(Src, Buf, (UINT32)Size, &Got);
            VfsClose(Src);
            if (Rc != SUCCESS || Got != Size) {
                MemoryFree(Buf);
                StdErrPrintf("rootsetup: read '%s' failed (%d, %u/%llu)\n",
                             RelPath, Rc, Got, Size);
                return IO_ERROR;
            }

            Rc = RootSetupWriteFile(DstDir, Name, Buf, (UINT32)Size);
            MemoryFree(Buf);
            if (Rc != SUCCESS) return Rc;
            (*Copied)++;
        }
    }

    return SUCCESS;
}

STATIC INT CmdDiskMgrRootSetup(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 3) {
        StdErrPrintf("Usage: diskmgr rootsetup <volume>\n");
        StdErrPrintf("  <volume> = disk or partition (SATA0, SATA0:2, ...)\n");
        StdErrPrintf("  Copies USER contents into /bin on that volume.\n");
        StdErrPrintf("  Requires 'USER' to be registered (not nomountuser).\n");
        return -1;
    }

    const CHAR *Name = ArgV[2];

    Disk *D = NULLPTR;
    Partition *P = NULLPTR;
    if (CmdResolveVolume(Name, &D, &P) == 0) {
        StdErrPrintf("Volume not found: %s\n", Name);
        return -1;
    }

    if (!D->Write) {
        StdErrPrintf("%s is read-only\n", D->Name);
        return -1;
    }

    UINT64 StartLba    = P ? P->StartLba    : 0;
    UINT64 SectorCount = P ? P->SectorCount : D->SectorCount;

    /* 1. USER must have been registered by MountUserIso.
     *    If nomountuser was set, it is intentionally absent. */
    Disk *UserDisk = DiskMgrFindDisk("USER");
    if (!UserDisk) {
        StdErrPrintf("rootsetup: 'USER' is not registered\n");
        StdErrPrintf("rootsetup: did you boot with 'nomountuser'?\n");
        return NOT_FOUND;
    }
    if (!UserDisk->Read) {
        StdErrPrintf("rootsetup: 'USER' has no read handler\n");
        return DEVICE_INVALID;
    }

    /* 2. Mount USER if needed. Never unmount it afterwards — USER is
     *    a system-wide resource; rootsetup only borrows it. */
    VfsInode *UserRoot = NULLPTR;
    CHAR UserProbe[] = "USER::/";
    if (VfsWalk(VfsRootInode, UserProbe, &UserRoot) != SUCCESS) {
        INT Rc = VfsMountExNoCd("iso9660", UserDisk, 0, UserDisk->SectorCount,
                            "USER", &UserRoot);
        if (Rc != SUCCESS) {
            StdErrPrintf("rootsetup: cannot mount USER (%d)\n", Rc);
            return Rc;
        }
        /* Leave it mounted on purpose. */
    }

    /* 3. Mount the target volume — unless it is already mounted. */
    const CHAR *FsName = VfsDetectFilesystemAt(D, StartLba);
    if (!FsName) {
        StdErrPrintf("rootsetup: cannot detect FS on %s at LBA %llu\n",
                     D->Name, StartLba);
        VfsInodeUnref(UserRoot);
        return NOT_FOUND;
    }

    VfsInode *DstRoot = NULLPTR;
    BOOL WeMountedDst = FALSE;

    CHAR DstProbe[DRIVE_NAME_MAX + 4];
    SnPrintf(DstProbe, sizeof(DstProbe), "%s::/", D->Name);

    if (VfsWalk(VfsRootInode, DstProbe, &DstRoot) == SUCCESS) {
    	StdOutPrintf("rootsetup: %s already mounted, reusing\n", D->Name);
    } else {
    	if (!D->Read) {
            StdErrPrintf("rootsetup: %s has no Read handler\n", D->Name);
            VfsInodeUnref(UserRoot);
           return DEVICE_INVALID;
        }

    	const CHAR *FsName = VfsDetectFilesystemAt(D, StartLba);
    	if (!FsName) {
            StdErrPrintf("rootsetup: cannot detect FS on %s at LBA %llu\n",
                     D->Name, StartLba);
            VfsInodeUnref(UserRoot);
            return NOT_FOUND;
    	}

    	INT Rc = VfsMountExNoCd(FsName, D, StartLba, SectorCount,
                            NULLPTR, &DstRoot);
    	if (Rc != SUCCESS) {
            StdErrPrintf("rootsetup: VfsMountEx(%s) failed (%d)\n", FsName, Rc);
            VfsInodeUnref(UserRoot);
            return Rc;
    	}
    	WeMountedDst = TRUE;
    }

    /* 4. Create /bin on the target. */
    VfsInode *BinDir = NULLPTR;
    INT Rc = RootSetupEnsureDir(DstRoot, "bin", &BinDir);
    if (Rc != SUCCESS) {
        StdErrPrintf("rootsetup: cannot create /bin (%d)\n", Rc);
        goto done;
    }

    /* 5. Copy USER -> /bin. */
    UINT32 Copied = 0;
    Rc = RootSetupCopyTree(UserRoot, BinDir, "", &Copied);
    if (Rc != SUCCESS) {
        StdErrPrintf("rootsetup: copy failed (%d)\n", Rc);
    } else {
        StdOutPrintf("rootsetup: %u file(s) copied to /bin\n", Copied);
    }

     /* 6. Ensure /etc exists. */
    VfsInode *EtcDir = NULLPTR;
    Rc = RootSetupEnsureDir(DstRoot, "etc", &EtcDir);
    if (Rc != SUCCESS) {
        StdErrPrintf("rootsetup: cannot create /etc (%d)\n", Rc);
        goto done;
    }

    /* 7. Write /etc/env.conf if it does not already exist. */
    {
        VfsInode *ExistingEnv = NULLPTR;
        if (VfsWalk(EtcDir, "env.conf", &ExistingEnv) == SUCCESS) {
            VfsInodeUnref(ExistingEnv);
            StdOutPrintf("rootsetup: /etc/env.conf already exists, left intact\n");
        } else {
            Rc = RootSetupWriteFile(EtcDir, "env.conf",
                                    GRootSetupEnvConf,
                                    (UINT32)StrLen(GRootSetupEnvConf));
            if (Rc != SUCCESS) {
                StdErrPrintf("rootsetup: cannot write /etc/env.conf (%d)\n", Rc);
                VfsInodeUnref(EtcDir);
                goto done;
            }
            StdOutPrintf("rootsetup: /etc/env.conf created\n");
        }
    }
    VfsInodeUnref(EtcDir);

done:
    if (BinDir) VfsInodeUnref(BinDir);
    VfsInodeUnref(DstRoot);

    /* Only unmount the target if we mounted it. */
    if (WeMountedDst) VfsUnmountDrive(D->Name);

    /* USER is intentionally left mounted. */
    VfsInodeUnref(UserRoot);

    if (Rc == SUCCESS) {
        StdOutPrintf("rootsetup: done\n");
        return 0;
    }
    return -1;
}

/* ============================================================================
 * Help
 * ============================================================================ */

STATIC VOID CmdDiskMgrHelp(VOID) {
    StdOutPrintf("\n");
    StdOutPrintf("=== DiskMgr Commands ===\n");
    StdOutPrintf("\n");
    StdOutPrintf("  diskmgr list                        List all disks\n");
    StdOutPrintf("  diskmgr info <disk>                 Show disk information\n");
    StdOutPrintf("  diskmgr parts <disk>                List partitions on disk\n");
    StdOutPrintf("\n");
    StdOutPrintf("  diskmgr init <disk> gpt             Initialize GPT\n");
    StdOutPrintf("\n");
    StdOutPrintf("  diskmgr create <disk> <size> <type> [flags]\n");
    StdOutPrintf("      MBR:  diskmgr create SATA0 100 FAT32\n");
    StdOutPrintf("      GPT:  diskmgr create NVME1 100 ext4 -gpt -label rootfs\n");
    StdOutPrintf("      Optional: -start <lba> (manual placement)\n");
    StdOutPrintf("\n");
    StdOutPrintf("  diskmgr delete <disk> <idx>         Delete partition\n");
    StdOutPrintf("  diskmgr label <disk>p<idx> <label>  Set GPT label\n");
    StdOutPrintf("\n");
    StdOutPrintf("  === DiskCfg ===\n");
    StdOutPrintf("  diskmgr cfg <volume>                Show disk.cfg for volume\n");
    StdOutPrintf("  diskmgr set <volume> <key> <value>  Change disk.cfg\n");
    StdOutPrintf("      keys: disk_name, is_root\n");
    StdOutPrintf("  diskmgr clear <volume>              Reset disk.cfg\n");
    StdOutPrintf("  diskmgr rename <volume> <new_name>  Shortcut for set disk_name\n");
    StdOutPrintf("  diskmgr setroot <volume>            Shortcut for set is_root=1\n");
    StdOutPrintf("  diskmgr rootsetup <volume>          Setup root filesystem\n");
    StdOutPrintf("  diskmgr unsetroot <volume>          Shortcut for set is_root=0\n");
    StdOutPrintf("\n");
    StdOutPrintf("  diskmgr help                        Show this help\n");
    StdOutPrintf("\n");
    StdOutPrintf("Partition types:\n");
    StdOutPrintf("  NTFS FAT32 FAT16 Linux EXT4 EFI SWAP\n");
    StdOutPrintf("\n");
    StdOutPrintf("Schemes:\n");
    StdOutPrintf("  MBR  — legacy, max 4 primary, 2 TB max\n");
    StdOutPrintf("  GPT  — modern, 128 partitions, > 2 TB, labels, GUIDs\n");
    StdOutPrintf("\n");
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
    if (StrCmp(SubCmd, "rootsetup") == 0) {
        return CmdDiskMgrRootSetup(ArgC, ArgV);
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

    StdErrPrintf("Unknown subcommand: %s\n", SubCmd);
    StdOutPrintf("Try 'diskmgr help' for usage\n");
    return -1;
}

INT CmdDiskMgrRegister(VOID) {
    return CmdRegisterBuiltin("diskmgr",
                              "Disk management utility (MBR + GPT)",
                              CmdDiskMgrHandler);
}