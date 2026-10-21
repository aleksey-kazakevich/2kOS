#include <Drivers/Disk/DiskMgr.h>
#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Basecon.h>
#include <Fs/Iso9660.h>
#include <Return.h>

/* ============================================================================
 * LOW-LEVEL IO
 * ============================================================================ */

STATIC INT IsoReadSectors(IsoContext *Ctx, UINT64 Lba, UINT32 Count, VOID *Buf) {
    if (!Ctx || !Ctx->Disk || !Ctx->Disk->Read)
        RETURN(NO_OBJECT);

    UINT32 DiskSectorSize = Ctx->Disk->SectorSize;
    if (DiskSectorSize == ISO_SECTOR_SIZE) {
        return Ctx->Disk->Read(Ctx->Disk,
                               Ctx->StartLba + Lba,
                               Count, Buf);
    }

    UINT64 ByteOffset = Lba * ISO_SECTOR_SIZE;
    UINT64 ByteCount  = (UINT64)Count * ISO_SECTOR_SIZE;

    UINT64 StartSector = ByteOffset / DiskSectorSize;
    UINT64 EndSector   = (ByteOffset + ByteCount + DiskSectorSize - 1) / DiskSectorSize;
    UINT32 SectorsToRead = (UINT32)(EndSector - StartSector);

    UINT8 *Temp = (UINT8 *)MemoryAllocate((USIZE)SectorsToRead * DiskSectorSize);
    if (!Temp) RETURN(NO_MEMORY);

    INT Rc = Ctx->Disk->Read(Ctx->Disk, Ctx->StartLba + StartSector,
                             SectorsToRead, Temp);
    if (Rc == SUCCESS) {
        UINT64 Offset = ByteOffset - StartSector * DiskSectorSize;
        MemCpy(Buf, Temp + Offset, (USIZE)ByteCount);
    }
    MemoryFree(Temp);
    return Rc;
}

/* ============================================================================
 * UCS-2 (Joliet)
 * ============================================================================ */

STATIC INT IsoUcs2BeToUtf8(const UINT8 *Src, INT SrcBytes,
                            CHAR *Dst, INT DstMax) {
    INT Out = 0;
    for (INT I = 0; I + 1 < SrcBytes; I += 2) {
        UINT16 Cp = ((UINT16)Src[I] << 8) | Src[I + 1];
        if (Cp == 0) break;

        if (Cp < 0x80) {
            if (Out + 1 >= DstMax) break;
            Dst[Out++] = (CHAR)Cp;
        } else if (Cp < 0x800) {
            if (Out + 2 >= DstMax) break;
            Dst[Out++] = (CHAR)(0xC0 | (Cp >> 6));
            Dst[Out++] = (CHAR)(0x80 | (Cp & 0x3F));
        } else {
            if (Out + 3 >= DstMax) break;
            Dst[Out++] = (CHAR)(0xE0 | (Cp >> 12));
            Dst[Out++] = (CHAR)(0x80 | ((Cp >> 6) & 0x3F));
            Dst[Out++] = (CHAR)(0x80 | (Cp & 0x3F));
        }
    }
    Dst[Out] = '\0';
    return Out;
}

/* ============================================================================
 * PVD VALIDATION
 * ============================================================================ */

STATIC BOOL IsoValidatePvd(IsoPvd *Pvd) {
    if (!Pvd) return FALSE;
    if (Pvd->Type != ISO_VD_TYPE_PRIMARY) return FALSE;
    if (MemCmp(Pvd->Identifier, ISO_PVD_ID, 5) != 0) return FALSE;
    if (Pvd->Version != ISO_PVD_VERSION) return FALSE;
    return TRUE;
}

STATIC BOOL IsoValidateSvd(IsoPvd *Svd) {
    if (!Svd) return FALSE;
    if (Svd->Type != ISO_VD_TYPE_SUPPLEMENTARY) return FALSE;
    if (MemCmp(Svd->Identifier, ISO_PVD_ID, 5) != 0) return FALSE;
    if (Svd->Version != ISO_PVD_VERSION) return FALSE;
    if (Svd->FileStructureVersion != 1) return FALSE;
    return TRUE;
}

/* ============================================================================
 * VOLUME DESCRIPTOR SCANNING
 * ============================================================================ */

STATIC VOID IsoScanVolumeDescriptors(IsoContext *Ctx) {
    UINT8 *Buf = (UINT8 *)MemoryAllocate(ISO_SECTOR_SIZE);
    if (!Buf) return;

    for (UINT32 Lba = ISO_PVD_LBA; Lba < ISO_PVD_LBA + 32; Lba++) {
        if (IsoReadSectors(Ctx, Lba, 1, Buf) != SUCCESS) break;

        UINT8 Type = Buf[0];

        if (Type == ISO_VD_TYPE_TERMINATOR) break;

        if (Type == ISO_VD_TYPE_SUPPLEMENTARY) {
            IsoPvd *Svd = (IsoPvd *)Buf;
            if (IsoValidateSvd(Svd)) {
                IsoDirRecord *Root = (IsoDirRecord *)Svd->RootDirRecord;
                if (Root->Length >= 33) {
                    MemCpy(&Ctx->Svd, Svd, sizeof(IsoPvd));
                    Ctx->HasJoliet = TRUE;
                    Ctx->JolietRootLba = Root->ExtentLbaLE;
                    Ctx->JolietRootSize = Root->DataLengthLE;
                    BaseconPrintf(BASECON_TYPE_NORMAL,
                                  "iso9660: Joliet SVD found at LBA %u\n", Lba);
                }
            }
        } else if (Type == ISO_VD_TYPE_BOOT) {
            /* Boot Record — may be El Torito */
            if (MemCmp(Buf + 7, EL_TORITO_ID, 23) == 0) {
                ElToritoDescriptor *Et = (ElToritoDescriptor *)Buf;
                Ctx->HasElTorito = TRUE;
                Ctx->BootCatalogLba = Et->BootCatalogLbaLE;
                BaseconPrintf(BASECON_TYPE_NORMAL,
                              "iso9660: El Torito boot catalog at LBA %u\n",
                              Ctx->BootCatalogLba);
            }
        }
    }

    MemoryFree(Buf);
}

/* ============================================================================
 * ROCK RIDGE: parsing SUSP/RRIP entries
 * ============================================================================ */

/* Find a SUSP entry by signature. Returns a pointer or NULLPTR. */
STATIC const UINT8 *SuspFindEntry(const UINT8 *Sua, UINT32 SuaLen,
                                   UINT16 Sig) {
    UINT32 Pos = 0;
    while (Pos + 4 <= SuaLen) {
        const UINT8 *Entry = Sua + Pos;
        UINT16 EntrySig = ((UINT16)Entry[0] << 8) | Entry[1];
        UINT8 EntryLen = Entry[2];

        if (EntrySig == Sig) return Entry;
        if (EntrySig == SUSP_ENTRY_ST) return NULLPTR;
        if (EntryLen < 4) break;

        Pos += EntryLen;
    }
    return NULLPTR;
}

/* Parses NM (Name) from SUA. Supports continuations (flag 0x01). */
STATIC BOOL RripParseName(const UINT8 *Sua, UINT32 SuaLen,
                           CHAR *Out, INT OutMax) {
    INT OutPos = 0;
    UINT32 Pos = 0;

    while (Pos + 4 <= SuaLen) {
        const UINT8 *Entry = Sua + Pos;
        UINT16 Sig = ((UINT16)Entry[0] << 8) | Entry[1];
        UINT8 Len = Entry[2];

        if (Sig == SUSP_ENTRY_ST) break;
        if (Len < 4) break;

        if (Sig == RRIP_ENTRY_NM) {
            UINT8 Flags = Entry[3];
            UINT8 NameLen = Len - 5;
            const UINT8 *Name = Entry + 5;

            for (UINT8 I = 0; I < NameLen && OutPos < OutMax - 1; I++) {
                Out[OutPos++] = (CHAR)Name[I];
            }

            if (!(Flags & NM_FLAG_CONTINUE)) break;
        }

        Pos += Len;
    }

    Out[OutPos] = '\0';
    return OutPos > 0;
}

/* Parses PX (POSIX attrs) from SUA. */
STATIC BOOL RripParsePosix(const UINT8 *Sua, UINT32 SuaLen,
                            UINT32 *OutMode, UINT32 *OutUid, UINT32 *OutGid,
                            UINT32 *OutNlink) {
    const UINT8 *Entry = SuspFindEntry(Sua, SuaLen, RRIP_ENTRY_PX);
    if (!Entry) return FALSE;
    if (Entry[2] < 36) return FALSE;   /* PX: 4 + 8 + 8 + 8 + 8 = 36 */

    /* Format: mode (8 bytes: LE + BE), nlink (8), uid (8), gid (8) */
    const UINT8 *P = Entry + 4;

    UINT32 ModeLE  = P[0] | (P[1] << 8) | (P[2] << 16) | (P[3] << 24);
    UINT32 NlinkLE = P[8] | (P[9] << 8) | (P[10] << 16) | (P[11] << 24);
    UINT32 UidLE   = P[16] | (P[17] << 8) | (P[18] << 16) | (P[19] << 24);
    UINT32 GidLE   = P[24] | (P[25] << 8) | (P[26] << 16) | (P[27] << 24);

    if (OutMode)  *OutMode = ModeLE;
    if (OutNlink) *OutNlink = NlinkLE;
    if (OutUid)   *OutUid = UidLE;
    if (OutGid)   *OutGid = GidLE;
    return TRUE;
}

/* Checks whether Rock Ridge is present in this SUA (by presence of ER or RR). */
STATIC BOOL SuspIsRockRidge(const UINT8 *Sua, UINT32 SuaLen) {
    /* Presence of an RR or ER entry is sufficient */
    if (SuspFindEntry(Sua, SuaLen, RRIP_ENTRY_ER)) return TRUE;
    if (SuspFindEntry(Sua, SuaLen, RRIP_ENTRY_RR)) return TRUE;
    return FALSE;
}

/* ============================================================================
 * DIRECTORY RECORD PARSING (with all extensions)
 * ============================================================================ */

typedef struct {
    UINT32 Lba;
    UINT32 Size;
    UINT8  Flags;
    CHAR   Name[256];       /* final name (RR > Joliet > ISO) */
    BOOL   IsDot;
    BOOL   IsDotDot;
    BOOL   FromRockRidge;
    BOOL   FromJoliet;
    BOOL   HasPosix;
    UINT32 PosixMode;
    UINT32 PosixUid;
    UINT32 PosixGid;
    UINT32 PosixNlink;
} IsoParsedEntry;

STATIC BOOL IsoParseDirRecordEx(IsoContext *Ctx,
                                 const UINT8 *Rec, UINT32 RecLen,
                                 BOOL PreferJoliet,
                                 IsoParsedEntry *Out) {
    if (!Rec || RecLen < 33 || !Out) return FALSE;
    MemSet(Out, 0, sizeof(*Out));

    IsoDirRecord *R = (IsoDirRecord *)Rec;
    Out->Lba = R->ExtentLbaLE;
    Out->Size = R->DataLengthLE;
    Out->Flags = R->Flags;

    UINT8 NameLen = R->FileIdentifierLength;
    if (NameLen == 0) return FALSE;

    const UINT8 *NamePtr = R->FileIdentifier;

    /* Dot / DotDot */
    if (NameLen == 1 && NamePtr[0] == 0x00) {
        Out->IsDot = TRUE;
        StrCpy(Out->Name, ".");
        return TRUE;
    }
    if (NameLen == 1 && NamePtr[0] == 0x01) {
        Out->IsDotDot = TRUE;
        StrCpy(Out->Name, "..");
        return TRUE;
    }

    /* Start with the ISO name — it is always present */
    INT J = 0;
    for (INT I = 0; I < NameLen && J < (INT)sizeof(Out->Name) - 1; I++) {
        UINT8 C = NamePtr[I];
        if (C == ';') break;
        if (C >= 'A' && C <= 'Z') C = C - 'A' + 'a';
        Out->Name[J++] = (CHAR)C;
    }
    Out->Name[J] = '\0';

    /* If this is a Joliet record and Joliet is preferred — overwrite */
    if (PreferJoliet) {
        CHAR JolietName[256];
        IsoUcs2BeToUtf8(NamePtr, NameLen, JolietName, sizeof(JolietName));

        /* Strip ";N" */
        CHAR *Semi = StrrChr(JolietName, ';');
        if (Semi) *Semi = '\0';

        StrnCpy(Out->Name, JolietName, sizeof(Out->Name) - 1);
        Out->FromJoliet = TRUE;
    }

    /* === Rock Ridge === */
    /* SUA starts right after FileIdentifier + padding */
    UINT32 FixedLen = 33;   /* up to FileIdentifierLength */
    UINT32 SuaOffset = FixedLen + NameLen;

    /* Padding: if NameLen is even, one byte is added */
    if (NameLen % 2 == 0) SuaOffset++;

    if (SuaOffset < RecLen && Ctx->HasRockRidge) {
        const UINT8 *Sua = Rec + SuaOffset;
        UINT32 SuaLen = RecLen - SuaOffset;

        /* Skip the SP entry if present */
        if (Ctx->SuspSkipBytes > 0 && Ctx->SuspSkipBytes <= SuaLen) {
            Sua += Ctx->SuspSkipBytes;
            SuaLen -= Ctx->SuspSkipBytes;
        }

        /* NM — alternative name */
        CHAR RrName[256];
        if (RripParseName(Sua, SuaLen, RrName, sizeof(RrName))) {
            /* RR names take priority over Joliet */
            StrnCpy(Out->Name, RrName, sizeof(Out->Name) - 1);
            Out->FromRockRidge = TRUE;
            Out->FromJoliet = FALSE;
        }

        /* PX — POSIX attrs */
        if (RripParsePosix(Sua, SuaLen,
                           &Out->PosixMode, &Out->PosixUid,
                           &Out->PosixGid, &Out->PosixNlink)) {
            Out->HasPosix = TRUE;
        }
    }

    return TRUE;
}

/* ============================================================================
 * ROCK RIDGE DETECTION IN ROOT
 * ============================================================================ */

STATIC VOID IsoDetectRockRidge(IsoContext *Ctx) {
    /* Read the root directory record from the PVD (first root sector) */
    UINT8 *Buf = (UINT8 *)MemoryAllocate(ISO_SECTOR_SIZE);
    if (!Buf) return;

    if (IsoReadSectors(Ctx, Ctx->RootLba, 1, Buf) != SUCCESS) {
        MemoryFree(Buf);
        return;
    }

    /* The first record in the root is the root itself (.), its SUA may contain SP */
    UINT32 Offset = 0;
    while (Offset < ISO_SECTOR_SIZE) {
        UINT8 RecLen = Buf[Offset];
        if (RecLen == 0) break;
        if (Offset + RecLen > ISO_SECTOR_SIZE) break;

        IsoDirRecord *R = (IsoDirRecord *)(Buf + Offset);
        UINT32 FixedLen = 33;
        UINT32 SuaOffset = FixedLen + R->FileIdentifierLength;
        if (R->FileIdentifierLength % 2 == 0) SuaOffset++;

        if (SuaOffset < RecLen) {
            const UINT8 *Sua = Buf + Offset + SuaOffset;
            UINT32 SuaLen = RecLen - SuaOffset;

            /* Check for the SP entry */
            const UINT8 *Sp = SuspFindEntry(Sua, SuaLen, SUSP_ENTRY_SP);
            if (Sp) {
                UINT8 SpLen = Sp[2];
                Ctx->HasSusp = TRUE;

                /* Bytes 6-7 of SP: skip for subsequent records */
                if (SpLen >= 7) {
                    Ctx->SuspSkipBytes = Sp[6];
                } else {
                    Ctx->SuspSkipBytes = 0;
                }
            }

            /* Check for RR/ER presence */
            if (SuspIsRockRidge(Sua, SuaLen)) {
                Ctx->HasRockRidge = TRUE;
            }
        }

        Offset += RecLen;
    }

    MemoryFree(Buf);

    if (Ctx->HasRockRidge) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "iso9660: Rock Ridge detected\n");
    }
}

/* ============================================================================
 * ROOT LBA/SIZE SELECTION
 * ============================================================================ */

STATIC VOID IsoSelectRoot(IsoContext *Ctx,
                          UINT32 *OutLba, UINT32 *OutSize, BOOL *OutJoliet) {
    if (Ctx->HasRockRidge) {
        /* Rock Ridge uses the PVD root (SUA in PVD-root) */
        *OutLba = Ctx->RootLba;
        *OutSize = Ctx->RootSize;
        *OutJoliet = FALSE;
        return;
    }
    if (Ctx->HasJoliet) {
        *OutLba = Ctx->JolietRootLba;
        *OutSize = Ctx->JolietRootSize;
        *OutJoliet = TRUE;
        return;
    }
    *OutLba = Ctx->RootLba;
    *OutSize = Ctx->RootSize;
    *OutJoliet = FALSE;
}

/* ============================================================================
 * VFS OPERATIONS
 * ============================================================================ */

STATIC VfsOperations GIsoNodeOps;
STATIC VfsFileOperations GIsoFileOps;

STATIC VfsInode *IsoAllocInode(IsoContext *Ctx, IsoParsedEntry *Entry,
                                UINT32 ParentLba, const CHAR *ForceName) {
    VfsInode *Node = VfsAllocInode();
    if (!Node) return NULLPTR;

    IsoInodePrivate *Priv = (IsoInodePrivate *)MemoryAllocate(sizeof(IsoInodePrivate));
    if (!Priv) { VfsFreeInode(Node); return NULLPTR; }
    MemSet(Priv, 0, sizeof(IsoInodePrivate));

    Priv->Ctx = Ctx;
    Priv->ExtentLba = Entry->Lba;
    Priv->DataLength = Entry->Size;
    Priv->ParentExtentLba = ParentLba;
    Priv->Flags = Entry->Flags;
    Priv->FromJoliet = Entry->FromJoliet;
    Priv->FromRockRidge = Entry->FromRockRidge;
    Priv->HasPosix = Entry->HasPosix;
    Priv->PosixMode = Entry->PosixMode;
    Priv->PosixUid = Entry->PosixUid;
    Priv->PosixGid = Entry->PosixGid;
    Priv->PosixNlink = Entry->PosixNlink;

    const CHAR *Name = ForceName ? ForceName : Entry->Name;
    StrnCpy(Priv->Name, Name, sizeof(Priv->Name) - 1);

    Node->IPrivate = Priv;
    Node->IDev = Ctx->Disk;
    Node->IOp = &GIsoNodeOps;
    Node->IFop = &GIsoFileOps;
    StrCpy(Node->IFsName, "iso9660");

    /* File mode */
    if (Entry->HasPosix) {
        Node->IMode = Entry->PosixMode & 0xFFFF;
    } else {
        Node->IMode = (Entry->Flags & ISO_FLAG_DIRECTORY) ? FT_DIR : FT_REG_FILE;
    }
    Node->ISize = Entry->Size;
    Node->IUid = Entry->HasPosix ? Entry->PosixUid : 0;
    Node->IGid = Entry->HasPosix ? Entry->PosixGid : 0;
    Node->INlink = Entry->HasPosix ? Entry->PosixNlink : 1;

    return Node;
}

STATIC INT IsoLookup(VfsInode *Dir, const CHAR *Name, VfsInode **Result) {
    if (!Dir || !Name || !Result) RETURN(INCORRECT_VALUE);

    IsoInodePrivate *DirPriv = (IsoInodePrivate *)Dir->IPrivate;
    if (!DirPriv) RETURN(NO_OBJECT);

    IsoContext *Ctx = DirPriv->Ctx;
    UINT32 DirLba = DirPriv->ExtentLba;
    UINT32 DirSize = DirPriv->DataLength;

    UINT8 *Buf = (UINT8 *)MemoryAllocate(ISO_SECTOR_SIZE);
    if (!Buf) RETURN(NO_MEMORY);

    INT Result2 = NOT_FOUND;

    UINT32 BytesRead = 0;
    UINT32 Sector = 0;

    while (BytesRead < DirSize) {
        INT Rc = IsoReadSectors(Ctx, DirLba + Sector, 1, Buf);
        if (Rc != SUCCESS) { Result2 = Rc; break; }
        Sector++;
        BytesRead += ISO_SECTOR_SIZE;

        UINT32 Offset = 0;
        while (Offset < ISO_SECTOR_SIZE) {
            UINT8 RecLen = Buf[Offset];
            if (RecLen == 0) break;
            if (Offset + RecLen > ISO_SECTOR_SIZE) break;

            IsoParsedEntry Entry;
            if (IsoParseDirRecordEx(Ctx, Buf + Offset, RecLen,
                                     DirPriv->FromJoliet, &Entry)) {
                if (!Entry.IsDot && !Entry.IsDotDot) {
                    /* Comparison: if RR — exact; otherwise — case-insensitive */
                    BOOL Match;
                    if (Entry.FromRockRidge) {
                        Match = (StrCmp(Entry.Name, Name) == 0);
                    } else if (Entry.FromJoliet) {
                        Match = (StrCmp(Entry.Name, Name) == 0);
                    } else {
                        Match = (StrCaseCmp(Entry.Name, Name) == 0);
                    }

                    if (Match) {
                        VfsInode *Node = IsoAllocInode(Ctx, &Entry, DirLba, NULLPTR);
                        if (!Node) { Result2 = NO_MEMORY; goto done; }
                        *Result = Node;
                        Result2 = SUCCESS;
                        goto done;
                    }
                }
            }
            Offset += RecLen;
        }
    }

done:
    MemoryFree(Buf);
    return Result2;
}

STATIC INT IsoRead(VfsInode *Inode, UINT64 Offset, VOID *Buf,
                   UINT32 Size, UINT32 *Read) {
    if (!Inode || !Buf || !Read) RETURN(INCORRECT_VALUE);

    IsoInodePrivate *Priv = (IsoInodePrivate *)Inode->IPrivate;
    if (!Priv) RETURN(NO_OBJECT);
    if (Priv->Flags & ISO_FLAG_DIRECTORY) RETURN(INCORRECT_VALUE);

    *Read = 0;
    if (Offset >= Priv->DataLength) RETURN(SUCCESS);

    UINT32 ToRead = Size;
    if (Offset + ToRead > Priv->DataLength) {
        ToRead = (UINT32)(Priv->DataLength - Offset);
    }

    IsoContext *Ctx = Priv->Ctx;
    UINT8 *Out = (UINT8 *)Buf;

    UINT64 FileLba = Priv->ExtentLba + (Offset / ISO_SECTOR_SIZE);
    UINT32 FileOff = (UINT32)(Offset % ISO_SECTOR_SIZE);
    UINT32 Remaining = ToRead;

    UINT8 *Temp = (UINT8 *)MemoryAllocate(ISO_SECTOR_SIZE);
    if (!Temp) RETURN(NO_MEMORY);

    while (Remaining > 0) {
        INT Rc = IsoReadSectors(Ctx, FileLba, 1, Temp);
        if (Rc != SUCCESS) { MemoryFree(Temp); RETURN(Rc); }

        UINT32 Chunk = ISO_SECTOR_SIZE - FileOff;
        if (Chunk > Remaining) Chunk = Remaining;

        MemCpy(Out, Temp + FileOff, Chunk);
        Out += Chunk;
        Remaining -= Chunk;
        FileOff = 0;
        FileLba++;
    }

    MemoryFree(Temp);
    *Read = ToRead;
    RETURN(SUCCESS);
}

STATIC INT IsoReadDir(VfsInode *Dir, UINT64 *Pos, CHAR *Name,
                      UINT32 *NameLen, UINT32 *Type) {
    if (!Dir || !Pos || !Name || !NameLen || !Type) RETURN(INCORRECT_VALUE);

    IsoInodePrivate *Priv = (IsoInodePrivate *)Dir->IPrivate;
    if (!Priv) RETURN(NO_OBJECT);
    if (!(Priv->Flags & ISO_FLAG_DIRECTORY)) RETURN(INCORRECT_VALUE);

    IsoContext *Ctx = Priv->Ctx;
    UINT64 TargetIndex = *Pos;
    UINT64 CurrentIndex = 0;

    UINT32 TotalSectors = (Priv->DataLength + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
    UINT8 *Buf = (UINT8 *)MemoryAllocate(ISO_SECTOR_SIZE);
    if (!Buf) RETURN(NO_MEMORY);

    INT Result = NOT_FOUND;

    for (UINT32 Sector = 0; Sector < TotalSectors; Sector++) {
        INT Rc = IsoReadSectors(Ctx, Priv->ExtentLba + Sector, 1, Buf);
        if (Rc != SUCCESS) { Result = Rc; break; }

        UINT32 Offset = 0;
        while (Offset < ISO_SECTOR_SIZE) {
            UINT8 RecLen = Buf[Offset];
            if (RecLen == 0) break;
            if (Offset + RecLen > ISO_SECTOR_SIZE) break;

            IsoParsedEntry Entry;
            if (IsoParseDirRecordEx(Ctx, Buf + Offset, RecLen,
                                     Priv->FromJoliet, &Entry)) {
                if (!Entry.IsDot && !Entry.IsDotDot) {
                    if (CurrentIndex == TargetIndex) {
                        StrnCpy(Name, Entry.Name, 255);
                        Name[255] = '\0';
                        *NameLen = (UINT32)StrLen(Name);
                        *Type = (Entry.Flags & ISO_FLAG_DIRECTORY) ?
                                FT_DIR : FT_REG_FILE;
                        (*Pos)++;
                        Result = SUCCESS;
                        goto done;
                    }
                    CurrentIndex++;
                }
            }
            Offset += RecLen;
        }
    }

done:
    MemoryFree(Buf);
    return Result;
}

STATIC INT IsoGetName(VfsInode *Inode, CHAR *Name, INT MaxLen) {
    if (!Inode || !Name || MaxLen <= 0) RETURN(INCORRECT_VALUE);

    IsoInodePrivate *Priv = (IsoInodePrivate *)Inode->IPrivate;
    if (!Priv) RETURN(NO_OBJECT);

    StrnCpy(Name, Priv->Name, MaxLen - 1);
    Name[MaxLen - 1] = '\0';
    RETURN(SUCCESS);
}

STATIC INT IsoParent(VfsInode *Inode, VfsInode **Parent) {
    if (!Inode || !Parent) RETURN(INCORRECT_VALUE);

    IsoInodePrivate *Priv = (IsoInodePrivate *)Inode->IPrivate;
    if (!Priv) RETURN(NO_OBJECT);

    IsoContext *Ctx = Priv->Ctx;

    UINT32 EffectiveRootLba;
    UINT32 EffectiveRootSize;
    BOOL FromJoliet;
    IsoSelectRoot(Ctx, &EffectiveRootLba, &EffectiveRootSize, &FromJoliet);

    if (Priv->ExtentLba == EffectiveRootLba) {
        *Parent = Inode;
        VfsInodeRef(Inode);
        RETURN(SUCCESS);
    }

    IsoParsedEntry RootEntry;
    MemSet(&RootEntry, 0, sizeof(RootEntry));
    RootEntry.Lba = EffectiveRootLba;
    RootEntry.Size = EffectiveRootSize;
    RootEntry.Flags = ISO_FLAG_DIRECTORY;
    RootEntry.FromJoliet = FromJoliet;
    StrCpy(RootEntry.Name, "..");

    VfsInode *ParentNode = IsoAllocInode(Ctx, &RootEntry,
                                          EffectiveRootLba, "..");
    if (!ParentNode) RETURN(NO_MEMORY);

    *Parent = ParentNode;
    RETURN(SUCCESS);
}

STATIC INT IsoStat(VfsInode *Inode, VOID *StatBuf) {
    return VfsStat(Inode, (VfsStats *)StatBuf);
}

STATIC INT IsoSync(VfsInode *Inode) {
    (VOID)Inode;
    RETURN(SUCCESS);
}

STATIC VOID IsoDestroy(VfsInode *Inode) {
    if (!Inode || !Inode->IPrivate) return;

    IsoInodePrivate *Priv = (IsoInodePrivate *)Inode->IPrivate;
    IsoContext *Ctx = Priv->Ctx;

    if (Ctx && Ctx->MountRoot == Inode) {
        Ctx->MountRoot = NULLPTR;
        MemoryFree(Ctx);
    }
    MemoryFree(Priv);
    Inode->IPrivate = NULLPTR;
}

STATIC INT IsoUnmount(VfsInode *Root) {
    (VOID)Root;
    RETURN(SUCCESS);
}

STATIC VfsOperations GIsoNodeOps = {
    .Lookup  = IsoLookup,
    .ReadDir = IsoReadDir,
    .GetName = IsoGetName,
    .Parent  = IsoParent,
    .Stat    = IsoStat,
    .Unmount = IsoUnmount,
    .Destroy = IsoDestroy,
};

STATIC VfsFileOperations GIsoFileOps = {
    .Read = IsoRead,
    .Sync = IsoSync,
};

/* ============================================================================
 * MOUNT
 * ============================================================================ */

STATIC INT IsoMount(Disk *Disk, UINT64 StartLba, UINT64 SectorCount, VfsInode **Root) {
    if (!Disk || !Disk->Read) RETURN(NO_OBJECT);

    IsoContext *Ctx = (IsoContext *)MemoryAllocate(sizeof(IsoContext));
    if (!Ctx) RETURN(NO_MEMORY);
    MemSet(Ctx, 0, sizeof(IsoContext));

    Ctx->Disk = Disk;
    Ctx->StartLba = StartLba;
    Ctx->SectorSize = ISO_SECTOR_SIZE;
    Ctx->TotalSectors = SectorCount ? SectorCount : Disk->SectorCount;

    /* Read the PVD */
    INT Rc = IsoReadSectors(Ctx, ISO_PVD_LBA, 1, &Ctx->Pvd);
    if (Rc != SUCCESS) { MemoryFree(Ctx); RETURN(Rc); }
    if (!IsoValidatePvd(&Ctx->Pvd)) {
        BaseconPrintf(BASECON_TYPE_ERROR, "iso9660: invalid PVD\n");
        MemoryFree(Ctx);
        RETURN(INCORRECT_VALUE);
    }

    IsoDirRecord *RootRec = (IsoDirRecord *)Ctx->Pvd.RootDirRecord;
    Ctx->RootLba = RootRec->ExtentLbaLE;
    Ctx->RootSize = RootRec->DataLengthLE;

    /* Scan VDs: look for Joliet (SVD) and El Torito (Boot Record) */
    IsoScanVolumeDescriptors(Ctx);

    /* Detect Rock Ridge */
    IsoDetectRockRidge(Ctx);

    /* Select root */
    UINT32 EffectiveRootLba;
    UINT32 EffectiveRootSize;
    BOOL FromJoliet;
    IsoSelectRoot(Ctx, &EffectiveRootLba, &EffectiveRootSize, &FromJoliet);

    /* Volume ID */
    CHAR VolumeId[33];
    MemCpy(VolumeId, Ctx->Pvd.VolumeId, 32);
    VolumeId[32] = '\0';
    for (INT I = 31; I >= 0 && VolumeId[I] == ' '; I--) VolumeId[I] = '\0';

    /* What are we using? */
    const CHAR *NameMode = "ISO9660";
    if (Ctx->HasRockRidge) NameMode = "RockRidge";
    else if (Ctx->HasJoliet) NameMode = "Joliet";

    /* Root inode */
    IsoParsedEntry RootEntry;
    MemSet(&RootEntry, 0, sizeof(RootEntry));
    RootEntry.Lba = EffectiveRootLba;
    RootEntry.Size = EffectiveRootSize;
    RootEntry.Flags = ISO_FLAG_DIRECTORY;
    RootEntry.FromJoliet = FromJoliet;

    VfsInode *RootNode = IsoAllocInode(Ctx, &RootEntry,
                                        EffectiveRootLba, "");
    if (!RootNode) { MemoryFree(Ctx); RETURN(NO_MEMORY); }

    Ctx->MountRoot = RootNode;
    *Root = RootNode;
    RETURN(SUCCESS);
}

/* ============================================================================
 * EL TORITO: get boot image
 * ============================================================================ */

INT Iso9660GetBootImage(IsoContext *Ctx, UINT32 *OutLba, UINT32 *OutSize) {
    if (!Ctx || !Ctx->HasElTorito || !OutLba || !OutSize)
        RETURN(NO_OBJECT);

    UINT8 *Buf = (UINT8 *)MemoryAllocate(ISO_SECTOR_SIZE);
    if (!Buf) RETURN(NO_MEMORY);

    INT Rc = IsoReadSectors(Ctx, Ctx->BootCatalogLba, 1, Buf);
    if (Rc != SUCCESS) { MemoryFree(Buf); RETURN(Rc); }

    ElToritoBootEntry *Entry = (ElToritoBootEntry *)Buf;

    /* Validity check */
    if (Entry->BootIndicator != 0x88) {
        MemoryFree(Buf);
        RETURN(INCORRECT_VALUE);
    }

    *OutLba = Entry->LoadLbaLE;
    *OutSize = Entry->SectorCount;   /* in 512-byte sectors */

    MemoryFree(Buf);
    RETURN(SUCCESS);
}

/* ============================================================================
 * REGISTRATION
 * ============================================================================ */

STATIC FileSystem GIsoFs = {
    .Name    = "iso9660",
    .Mount   = IsoMount,
    .Unmount = IsoUnmount,
    .Next    = NULLPTR,
};

INT Iso9660Init(VOID) {
    VfsRegisterFs(&GIsoFs);
    RETURN(SUCCESS);
}