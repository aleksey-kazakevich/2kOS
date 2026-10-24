#include <Drivers/Disk/DiskMgr.h>
#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Io/StdErr.h>
#include <Fs/Fat32.h>
#include <Fs/Exfat.h>
#include <Return.h>
#include <Fs/Vfs.h>

/* ============================================================================
 * INTERNAL HELPERS
 * ============================================================================ */

STATIC INLINE BOOL DiskCfg_IsSpace(CHAR C) {
    return C == ' ' || C == '\t' || C == '\r';
}

STATIC USIZE DiskCfg_TrimRight(const CHAR *S, USIZE Len) {
    while (Len > 0) {
        CHAR C = S[Len - 1];
        if (C == '\n' || C == '\r' || C == ' ' || C == '\t') Len--;
        else break;
    }
    return Len;
}

STATIC BOOL DiskCfg_ParseKV(const CHAR *Line, USIZE Len,
                             CHAR *Key, USIZE KeyMax,
                             CHAR *Val, USIZE ValMax) {
    USIZE I = 0, K = 0, V = 0;

    while (I < Len && DiskCfg_IsSpace(Line[I])) I++;
    while (I < Len && Line[I] != '=' && K < KeyMax - 1) {
        if (DiskCfg_IsSpace(Line[I])) { I++; continue; }
        Key[K++] = Line[I++];
    }
    Key[K] = '\0';
    if (I >= Len || Line[I] != '=') return FALSE;
    I++;
    while (I < Len && DiskCfg_IsSpace(Line[I])) I++;

    if (I < Len && Line[I] == '"') {
        I++;
        while (I < Len && Line[I] != '"' && V < ValMax - 1)
            Val[V++] = Line[I++];
        if (I >= Len || Line[I] != '"') return FALSE;
    } else {
        while (I < Len && !DiskCfg_IsSpace(Line[I]) && V < ValMax - 1)
            Val[V++] = Line[I++];
    }
    Val[V] = '\0';
    return K > 0;
}

/* ============================================================================
 * PARSE / SERIALIZE
 * ============================================================================ */

INT DiskCfgParse(const CHAR *Buf, USIZE Len, DiskCfg *Out) {
    if (!Buf || !Out) RETURN(INCORRECT_VALUE);

    MemSet(Out, 0, sizeof(DiskCfg));
    Out->Version = DISKCFG_VERSION;

    USIZE Pos = 0;
    while (Pos < Len) {
        USIZE Start = Pos;
        while (Pos < Len && Buf[Pos] != '\n') Pos++;
        USIZE LineLen = DiskCfg_TrimRight(Buf + Start, Pos - Start);
        Pos++;

        if (LineLen == 0) continue;
        if (Buf[Start] == '#') continue;

        CHAR Key[32], Val[64];
        if (!DiskCfg_ParseKV(Buf + Start, LineLen,
                             Key, sizeof(Key), Val, sizeof(Val)))
            continue;

        if (StrCaseCmp(Key, "version") == 0) {
            Out->Version = (UINT32)AToI(Val);
        } else if (StrCaseCmp(Key, "id") == 0) {
            StrnCpy(Out->Id, Val, sizeof(Out->Id) - 1);
        } else if (StrCaseCmp(Key, "disk_name") == 0) {
            StrnCpy(Out->Name, Val, sizeof(Out->Name) - 1);
        } else if (StrCaseCmp(Key, "is_root") == 0) {
            Out->IsRoot = (AToI(Val) != 0);
        }
    }

    Out->Valid = TRUE;
    RETURN(SUCCESS);
}

INT DiskCfgSerialize(const DiskCfg *Cfg, CHAR *Buf, USIZE BufSize) {
    if (!Cfg || !Buf || BufSize < 64) RETURN(INCORRECT_VALUE);

    INT N = SnPrintf(Buf, BufSize,
        "# disk.cfg - do not edit manually\n"
        "version=%u\n"
        "id=%s\n"
        "disk_name=\"%s\"\n"
        "is_root=%u\n",
        Cfg->Version ? Cfg->Version : DISKCFG_VERSION,
        Cfg->Id[0] ? Cfg->Id : "",
        Cfg->Name[0] ? Cfg->Name : "",
        Cfg->IsRoot ? 1 : 0);

    RETURN(N);
}

/* ============================================================================
 * PERSISTENT ID
 * ============================================================================ */

STATIC VOID DiskCfg_HexEncode(const UINT8 *Data, USIZE Len, char *Out) {
    STATIC const char Hex[] = "0123456789abcdef";
    for (USIZE I = 0; I < Len; I++) {
        Out[I * 2]     = Hex[Data[I] >> 4];
        Out[I * 2 + 1] = Hex[Data[I] & 0xF];
    }
    Out[Len * 2] = '\0';
}

VOID DiskCfgComputeId(Disk *D, UINT64 StartLba, UINT64 SectorCount,
                      DiskCfgFsKind Fs, char *Out, USIZE OutSize) {
    if (!D || !Out || OutSize < 8) return;

    if (D->PartScheme == DISK_PART_SCHEME_GPT) {
        BOOL Zero = TRUE;
        for (INT I = 0; I < 16; I++) {
            if (D->DiskGuid[I] != 0) { Zero = FALSE; break; }
        }
        if (!Zero) {
            SnPrintf(Out, OutSize, "GPT:");
            USIZE PrefixLen = StrLen(Out);
            DiskCfg_HexEncode(D->DiskGuid, 16, Out + PrefixLen);
            return;
        }
    }

    if (D->Read) {
        UINT8 Sector[512];
        if (D->Read(D, StartLba, 1, Sector) == SUCCESS) {
            if (Fs == DISKCFG_FS_FAT32) {
                Fat32Bpb *Bpb = (Fat32Bpb*)Sector;
                SnPrintf(Out, OutSize, "FAT:%08X", Bpb->VolumeId);
                return;
            }
            if (Fs == DISKCFG_FS_EXFAT) {
                ExfatVbr *Vbr = (ExfatVbr*)Sector;
                SnPrintf(Out, OutSize, "EXF:%08X", Vbr->VolumeSerial);
                return;
            }
            if (Fs == DISKCFG_FS_ISO9660) {
                SnPrintf(Out, OutSize, "ISO:%08X", (UINT32)SectorCount);
                return;
            }
        }
    }

    UINT64 H = 0x9E3779B97F4A7C15ULL;
    H ^= D->TotalSize;
    H *= 0xBF58476D1CE4E5B9ULL;
    H ^= StartLba * 0x94D049BB133111EBULL;
    H ^= ((UINT64)D->SectorSize) << 32;
    SnPrintf(Out, OutSize, "GEN:%016llx", H);
}

/* ============================================================================
 * READ VOLUME — via temporary VfsMount
 * ============================================================================ */

STATIC const CHAR *DiskCfg_FsKindToName(DiskCfgFsKind Fs) {
    switch (Fs) {
        case DISKCFG_FS_FAT32:   return "fat32";
        case DISKCFG_FS_EXFAT:   return "exfat";
        case DISKCFG_FS_ISO9660: return "iso9660";
        default:                 return NULLPTR;
    }
}

/* Temporary mount → read → unmount. Returns the contents of the file. */
STATIC INT DiskCfg_ReadFileTemporary(Disk *D, UINT64 StartLba, UINT64 SectorCount,
                                      DiskCfgFsKind Fs,
                                      UINT8 *Out, UINT32 OutSize, UINT32 *OutLen) {
    const CHAR *FsName = DiskCfg_FsKindToName(Fs);
    if (!FsName) RETURN(NOT_SUPPORTED);

    /* Saving the CWD (VfsMount makes VfsCd) */
    VfsInode *SavedCwd = VfsCurrentDir;
    if (SavedCwd) VfsInodeRef(SavedCwd);
    CHAR SavedPath[PATH_MAX];
    StrnCpy(SavedPath, VfsCurrentPath, PATH_MAX - 1);
    CHAR SavedDrive[DRIVE_NAME_MAX];
    StrnCpy(SavedDrive, VfsCurrentDrive, DRIVE_NAME_MAX - 1);

    VfsInode *Root = NULLPTR;
    INT Rc = VfsMount(FsName, D, StartLba, SectorCount, &Root);
    if (Rc != SUCCESS) {
        if (SavedCwd) VfsInodeUnref(SavedCwd);
        RETURN(Rc);
    }

    VfsFile *F = NULLPTR;
    Rc = VfsOpen(Root, DISKCFG_FILE_NAME, VFS_O_READ, &F);
    if (Rc != SUCCESS) {
        VfsUnmount(Root);
        if (SavedCwd) {
            VfsInode *Cur = VfsCurrentDir;
            VfsCurrentDir = SavedCwd;
            if (Cur && Cur != SavedCwd) {
                VfsInodeUnref(Cur);
            }
            StrnCpy(VfsCurrentPath, SavedPath, PATH_MAX - 1);
            StrnCpy(VfsCurrentDrive, SavedDrive, DRIVE_NAME_MAX - 1);
        }
        RETURN(Rc);
    }

    UINT32 Rd = 0;
    Rc = VfsRead(F, Out, OutSize, &Rd);
    VfsClose(F);

    VfsUnmount(Root);

    if (SavedCwd) {
        VfsInode *Cur = VfsCurrentDir;
        VfsCurrentDir = SavedCwd;
	if (Cur && Cur != SavedCwd) {
            VfsInodeUnref(Cur);
    	}
        StrnCpy(VfsCurrentPath, SavedPath, PATH_MAX - 1);
        StrnCpy(VfsCurrentDrive, SavedDrive, DRIVE_NAME_MAX - 1);
    }

    if (Rc == SUCCESS) *OutLen = Rd;
    RETURN(Rc);
}

INT DiskCfgReadVolume(Disk *D, UINT64 StartLba, UINT64 SectorCount,
                      DiskCfgFsKind Fs, DiskCfg *Out) {
    if (!D || !Out) RETURN(INCORRECT_VALUE);

    MemSet(Out, 0, sizeof(DiskCfg));
    Out->FsKind = Fs;

    if (D->Type == DISK_TYPE_RAM) RETURN(NOT_FOUND);

    if (Fs != DISKCFG_FS_FAT32 && Fs != DISKCFG_FS_EXFAT)
        RETURN(NOT_SUPPORTED);

    UINT8 Buf[1024];
    UINT32 Len = 0;
    INT Rc = DiskCfg_ReadFileTemporary(D, StartLba, SectorCount, Fs,
                                       Buf, sizeof(Buf), &Len);
    if (Rc != SUCCESS) RETURN(Rc);

    Rc = DiskCfgParse((const CHAR*)Buf, Len, Out);
    if (Rc != SUCCESS) RETURN(Rc);

    char RealId[DISKCFG_ID_MAX];
    DiskCfgComputeId(D, StartLba, SectorCount, Fs, RealId, sizeof(RealId));

    if (Out->Id[0] && StrCmp(Out->Id, RealId) != 0) {
        Out->Stale = TRUE;
    }
    if (!Out->Id[0]) {
        StrnCpy(Out->Id, RealId, sizeof(Out->Id) - 1);
    }

    RETURN(SUCCESS);
}

/* ============================================================================
 * WRITE VOLUME — via temporary VfsMount
 * ============================================================================ */

INT DiskCfgWriteVolume(Disk *D, UINT64 StartLba, UINT64 SectorCount,
                       DiskCfgFsKind Fs, const CHAR *Name, BOOL IsRoot) {
    if (!D || !D->Write) RETURN(NO_OBJECT);

    DiskCfg Cfg;
    MemSet(&Cfg, 0, sizeof(Cfg));
    Cfg.Version = DISKCFG_VERSION;
    DiskCfgComputeId(D, StartLba, SectorCount, Fs, Cfg.Id, sizeof(Cfg.Id));
    if (Name) StrnCpy(Cfg.Name, Name, sizeof(Cfg.Name) - 1);
    Cfg.IsRoot = IsRoot;

    CHAR Buf[512];
    INT Len = DiskCfgSerialize(&Cfg, Buf, sizeof(Buf));
    if (Len <= 0) {
        StdErrPrintf("diskcg: serialize failed (%d)\n", Len);
        RETURN(INCORRECT_VALUE);
    }

    const CHAR *FsName = DiskCfg_FsKindToName(Fs);
    if (!FsName) RETURN(NOT_SUPPORTED);

    VfsInode *SavedCwd = VfsCurrentDir;
    if (SavedCwd) VfsInodeRef(SavedCwd);
    CHAR SavedPath[PATH_MAX];
    StrnCpy(SavedPath, VfsCurrentPath, PATH_MAX - 1);
    CHAR SavedDrive[DRIVE_NAME_MAX];
    StrnCpy(SavedDrive, VfsCurrentDrive, DRIVE_NAME_MAX - 1);

    VfsInode *Root = NULLPTR;
    INT Rc = VfsMount(FsName, D, StartLba, SectorCount, &Root);
    if (Rc != SUCCESS) {
        StdErrPrintf("diskcg: VfsMount(%s) failed (%d)\n", FsName, Rc);
        if (SavedCwd) VfsInodeUnref(SavedCwd);
        RETURN(Rc);
    }

    VfsFile *F = NULLPTR;
    Rc = VfsOpen(Root, DISKCFG_FILE_NAME,
                 VFS_O_WRITE | VFS_O_CREAT | VFS_O_TRUNC, &F);
    if (Rc != SUCCESS) {
        StdErrPrintf("diskcg: VfsOpen failed (%d)\n", Rc);
        VfsUnmount(Root);
        if (SavedCwd) {
            VfsInode *Cur = VfsCurrentDir;
            VfsCurrentDir = SavedCwd;
            if (Cur && Cur != SavedCwd) {
                VfsInodeUnref(Cur);
            }
            StrnCpy(VfsCurrentPath, SavedPath, PATH_MAX - 1);
            StrnCpy(VfsCurrentDrive, SavedDrive, DRIVE_NAME_MAX - 1);
        }
        RETURN(Rc);
    }

    UINT32 Wr = 0;
    Rc = VfsWrite(F, Buf, (UINT32)Len, &Wr);
    VfsClose(F);

    VfsUnmount(Root);

    if (SavedCwd) {
    	VfsInode *Cur = VfsCurrentDir;
    	VfsCurrentDir = SavedCwd;
    	if (Cur && Cur != SavedCwd) {
            VfsInodeUnref(Cur);
    	}
    	StrnCpy(VfsCurrentPath, SavedPath, PATH_MAX - 1);
    	StrnCpy(VfsCurrentDrive, SavedDrive, DRIVE_NAME_MAX - 1);
    }

    if (Rc != SUCCESS || Wr != (UINT32)Len) {
        StdErrPrintf("diskcg: VfsWrite failed (rc=%d wr=%u/%d)\n",
                      Rc, Wr, Len);
        RETURN(INCORRECT_VALUE);
    }

    RETURN(SUCCESS);
}