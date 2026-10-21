#include <Drivers/Disk/DiskMgr.h>
#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Scheduler.h>
#include <Lib/StdIo.h>
#include <Fs/Exfat.h>
#include <Basecon.h>
#include <Return.h>

/* ============================================================================
 * Internal Functions
 * ============================================================================ */

STATIC INT ExfatReadSectors(ExfatContext *Ctx, UINT64 Sector, UINT32 Count, VOID *Buf) {
    return Ctx->Disk->Read(Ctx->Disk, Ctx->StartLba + Sector, Count, Buf);
}

STATIC INT ExfatWriteSectors(ExfatContext *Ctx, UINT64 Sector, UINT32 Count, const VOID *Buf) {
    return Ctx->Disk->Write(Ctx->Disk, Ctx->StartLba + Sector, Count, Buf);
}

STATIC INT ExfatReadCluster(ExfatContext *Ctx, UINT32 Cluster, UINT8 *Buffer) {
    if (!Ctx || !Buffer) return -1;
    if (Cluster < 2) return -1;
    if (Cluster >= Ctx->FatEntries) return -1;
    
    UINT64 Sector = Ctx->ClusterHeapStart / Ctx->BytesPerSector + 
                    (Cluster - 2) * Ctx->SectorsPerCluster;
    
    UINT64 MaxSector = Ctx->Disk->SectorCount;
    UINT64 EndSector = Sector + Ctx->SectorsPerCluster;
    if (EndSector > MaxSector) return -1;
    
    return ExfatReadSectors(Ctx, Sector, Ctx->SectorsPerCluster, Buffer);
}

STATIC INT ExfatWriteCluster(ExfatContext *Ctx, UINT32 Cluster, const UINT8 *Buffer) {
    if (Cluster < 2) return -1;
    
    UINT64 Sector = Ctx->ClusterHeapStart / Ctx->BytesPerSector + 
                    (Cluster - 2) * Ctx->SectorsPerCluster;
    
    return ExfatWriteSectors(Ctx, Sector, Ctx->SectorsPerCluster, Buffer);
}

STATIC UINT32 ExfatNextCluster(ExfatContext *Ctx, UINT32 Cluster) {
    if (Cluster >= Ctx->FatEntries) return 0;
    return Ctx->FatCache[Cluster];
}

#define EXFAT_DIR_SLOT_SIZE           32
#define EXFAT_MAX_SECONDARY_COUNT     18
#define EXFAT_MAX_DIR_CHAIN_ITER      4096

STATIC UINT16 ExfatChecksumDirSet(const UINT8 *Entries, UINT8 SecondaryCount) {
    UINT16 Sum = 0;
    UINT32 Bytes = (UINT32)(SecondaryCount + 1) * EXFAT_DIR_SLOT_SIZE;
    UINT32 I;

    for (I = 0; I < Bytes; I++) {
        Sum = ((Sum & 1) ? 0x8000 : 0) + (Sum >> 1) + Entries[I];
    }
    return Sum;
}

STATIC UINT8 ExfatSecondaryCountForName(const CHAR *Name) {
    UINT32 Len = StrLen(Name);
    UINT8 NameEntries = (UINT8)((Len + 14) / 15);

    if (NameEntries < 1) {
        NameEntries = 1;
    }
    return (UINT8)(NameEntries + 1);
}

STATIC UINT8 ExfatDirSlotsNeeded(const CHAR *Name) {
    return (UINT8)(1 + ExfatSecondaryCountForName(Name));
}

STATIC UINT8 *ExfatDirAdvance(UINT8 *Ptr, UINT8 *End, BOOL *DirEnded) {
    UINT8 Type;
    UINT8 Count;
    USIZE Step;

    if (Ptr >= End) {
        return End;
    }

    Type = Ptr[0];
    if (Type == 0) {
        if (DirEnded) {
            *DirEnded = TRUE;
        }
        return End;
    }

    if (Type == EXFAT_ENTRY_FILE) {
        Count = Ptr[1];
        if (Count > EXFAT_MAX_SECONDARY_COUNT) {
            if (DirEnded) {
                *DirEnded = TRUE;
            }
            return End;
        }
        Step = (USIZE)(Count + 1) * EXFAT_DIR_SLOT_SIZE;
        if (Ptr + Step > End) {
            if (DirEnded) {
                *DirEnded = TRUE;
            }
            return End;
        }
        return Ptr + Step;
    }

    if (Ptr + EXFAT_DIR_SLOT_SIZE > End) {
        if (DirEnded) {
            *DirEnded = TRUE;
        }
        return End;
    }
    return Ptr + EXFAT_DIR_SLOT_SIZE;
}

STATIC UINT32 ExfatDirLastCluster(ExfatContext *Ctx, UINT32 FirstCluster) {
    UINT32 Cluster = FirstCluster;
    UINT32 Next;
    UINT32 Iters = 0;

    if (Cluster < 2) {
        return 0;
    }

    while (Iters++ < EXFAT_MAX_DIR_CHAIN_ITER) {
        Next = ExfatNextCluster(Ctx, Cluster);
        if (Next < 2 || Next == EXFAT_FAT_END) {
            return Cluster;
        }
        Cluster = Next;
    }
    return Cluster;
}

STATIC INT ExfatAllocCluster(ExfatContext *Ctx, UINT32 *OutCluster) {
    UINT32 I;

    for (I = 2; I < Ctx->FatEntries; I++) {
        if (Ctx->FatCache[I] == EXFAT_FAT_FREE) {
            UINT8 *Zero;

            Ctx->FatCache[I] = EXFAT_FAT_END;
            Ctx->FatCache[0] |= 1;
            *OutCluster = I;

            Zero = (UINT8*)MemoryAllocate(Ctx->BytesPerCluster);
            if (Zero) {
                MemSet(Zero, 0, Ctx->BytesPerCluster);
                ExfatWriteCluster(Ctx, I, Zero);
                MemoryFree(Zero);
            }
            return 0;
        }
    }
    return -1;
}

STATIC INT ExfatExtendDirCluster(ExfatContext *Ctx, UINT32 DirFirstCluster, UINT32 *OutCluster) {
    UINT32 Tail;
    UINT32 NewCluster;

    Tail = ExfatDirLastCluster(Ctx, DirFirstCluster);
    if (Tail < 2) {
        return -1;
    }
    if (ExfatAllocCluster(Ctx, &NewCluster) != 0) {
        return -1;
    }
    Ctx->FatCache[Tail] = NewCluster;
    Ctx->FatCache[0] |= 1;
    *OutCluster = NewCluster;
    return 0;
}

STATIC UINT8 *ExfatFindFreeSlotsInBuf(UINT8 *Buf, UINT32 BufSize, UINT8 SlotsNeeded) {
    UINT8 *Ptr = Buf;
    UINT8 *End = Buf + BufSize;
    BOOL DirEnded = FALSE;

    while (Ptr < End && !DirEnded) {
        if (Ptr[0] == 0) {
            if (Ptr + (USIZE)SlotsNeeded * EXFAT_DIR_SLOT_SIZE <= End) {
                return Ptr;
            }
            return NULLPTR;
        }
        Ptr = ExfatDirAdvance(Ptr, End, &DirEnded);
    }
    return NULLPTR;
}

STATIC INT ExfatFindOrMakeDirSlot(ExfatPrivate *DirPriv, const CHAR *Name,
                                   UINT32 *OutCluster, UINT32 *OutEntry,
                                   UINT8 **OutBuf) {
    ExfatContext *Ctx = DirPriv->Exfat;
    UINT32 Cluster = DirPriv->FirstCluster;
    UINT32 Next;
    UINT32 Iters = 0;
    UINT8 Slots = ExfatDirSlotsNeeded(Name);
    UINT8 *Buf;
    UINT8 *Slot;

    while (Cluster >= 2 && Cluster != EXFAT_FAT_END && Iters++ < EXFAT_MAX_DIR_CHAIN_ITER) {
        Buf = (UINT8*)MemoryAllocate(Ctx->BytesPerCluster);
        if (!Buf) {
            return -1;
        }
        if (ExfatReadCluster(Ctx, Cluster, Buf) != 0) {
            MemoryFree(Buf);
            return -1;
        }

        Slot = ExfatFindFreeSlotsInBuf(Buf, Ctx->BytesPerCluster, Slots);
        if (Slot) {
            *OutCluster = Cluster;
            *OutEntry = (UINT32)((Slot - Buf) / EXFAT_DIR_SLOT_SIZE);
            *OutBuf = Buf;
            return 0;
        }

        MemoryFree(Buf);
        Next = ExfatNextCluster(Ctx, Cluster);
        if (Next < 2 || Next == EXFAT_FAT_END) {
            break;
        }
        Cluster = Next;
    }

    if (ExfatExtendDirCluster(Ctx, DirPriv->FirstCluster, &Cluster) != 0) {
        return -1;
    }

    Buf = (UINT8*)MemoryAllocate(Ctx->BytesPerCluster);
    if (!Buf) {
        return -1;
    }
    MemSet(Buf, 0, Ctx->BytesPerCluster);
    *OutCluster = Cluster;
    *OutEntry = 0;
    *OutBuf = Buf;
    return 0;
}

STATIC BOOL ExfatParseFileEntryName(UINT8 *FilePtr, UINT8 *End, CHAR *AsciiName, INT AsciiMax) {
    ExfatFileEntry *File = (ExfatFileEntry*)FilePtr;
    UINT8 SecondaryCount = File->SecondaryCount;
    UINT8 *StreamPtr;
    UINT8 *NamePtr;
    UINT8 *SetEnd;
    INT NameLen = 0;
    INT I;
    INT J;

    if (SecondaryCount > EXFAT_MAX_SECONDARY_COUNT) {
        return FALSE;
    }

    SetEnd = FilePtr + (USIZE)(SecondaryCount + 1) * EXFAT_DIR_SLOT_SIZE;
    if (SetEnd > End) {
        return FALSE;
    }

    StreamPtr = FilePtr + EXFAT_DIR_SLOT_SIZE;
    for (I = 0; I < SecondaryCount; I++) {
        if (StreamPtr[0] == EXFAT_ENTRY_STREAM) {
            break;
        }
        StreamPtr += EXFAT_DIR_SLOT_SIZE;
    }
    if (StreamPtr[0] != EXFAT_ENTRY_STREAM) {
        return FALSE;
    }

    NamePtr = StreamPtr + EXFAT_DIR_SLOT_SIZE;
    for (I = 0; I < SecondaryCount - 1; I++) {
        if (NamePtr[0] == EXFAT_ENTRY_NAME) {
            ExfatNameEntry *NameEntry = (ExfatNameEntry*)NamePtr;
            for (J = 0; J < 15; J++) {
                UINT16 C = NameEntry->Name[J];
                if (C == 0) {
                    break;
                }
                if (C < 128 && NameLen < AsciiMax - 1) {
                    AsciiName[NameLen++] = (CHAR)C;
                }
            }
        }
        NamePtr += EXFAT_DIR_SLOT_SIZE;
    }
    AsciiName[NameLen] = '\0';
    return TRUE;
}

STATIC VOID ExfatAttachParent(ExfatPrivate *Priv, VfsInode *Dir) {
    if (!Priv)
        return;
    Priv->ParentInode = Dir;
    if (Dir)
        VfsInodeRef(Dir);
}

STATIC VOID ExfatDestroy(VfsInode *Inode) {
    ExfatPrivate *Priv;
    ExfatContext *Ctx;

    if (!Inode || !Inode->IPrivate)
        return;

    Priv = (ExfatPrivate *)Inode->IPrivate;
    Ctx = Priv->Exfat;

    if (Priv->ParentInode) {
        VfsInodeUnref(Priv->ParentInode);
        Priv->ParentInode = NULLPTR;
    }

    if (Ctx && Ctx->MountRoot == Inode) {
        Ctx->MountRoot = NULLPTR;
        if (Ctx->FatCache)
            MemoryFree(Ctx->FatCache);
        MemoryFree(Ctx);
        Priv->Exfat = NULLPTR;
    }

    MemoryFree(Priv);
    Inode->IPrivate = NULLPTR;
}

STATIC ExfatStreamEntry *ExfatFindStreamEntry(UINT8 *FilePtr, UINT8 SecondaryCount) {
    UINT8 *StreamPtr = FilePtr + EXFAT_DIR_SLOT_SIZE;
    INT I;

    for (I = 0; I < SecondaryCount; I++) {
        if (StreamPtr[0] == EXFAT_ENTRY_STREAM) {
            return (ExfatStreamEntry*)StreamPtr;
        }
        StreamPtr += EXFAT_DIR_SLOT_SIZE;
    }
    return NULLPTR;
}

/* ============================================================================
 * VFS Operations
 * ============================================================================ */

STATIC INT ExfatLookup(VfsInode *Dir, const CHAR *Name, VfsInode **Result) {
    ExfatPrivate *DirPriv;
    ExfatContext *Ctx;
    UINT32 Cluster;
    UINT32 ChainIters;
    UINT8 *Buf;
    UINT8 *Ptr;
    UINT8 *End;
    BOOL DirEnded;
    INT Ret = -1;

    if (!Dir || !Name || !Result) {
        return -1;
    }
    if (Dir->IMode != FT_DIR) {
        return -1;
    }

    DirPriv = (ExfatPrivate*)Dir->IPrivate;
    if (!DirPriv || !DirPriv->Exfat) {
        return -1;
    }

    Ctx = DirPriv->Exfat;
    Cluster = DirPriv->FirstCluster;
    ChainIters = 0;

    Buf = (UINT8*)MemoryAllocate(Ctx->BytesPerCluster);
    if (!Buf) {
        return -1;
    }

    while (Cluster >= 2 && Cluster != EXFAT_FAT_END &&
           ChainIters++ < EXFAT_MAX_DIR_CHAIN_ITER) {
        if (ExfatReadCluster(Ctx, Cluster, Buf) != 0) {
            break;
        }

        Ptr = Buf;
        End = Buf + Ctx->BytesPerCluster;
        DirEnded = FALSE;

        while (Ptr < End && !DirEnded) {
            UINT8 Type = Ptr[0];

            if (Type == EXFAT_ENTRY_FILE) {
                ExfatFileEntry *File = (ExfatFileEntry*)Ptr;
                ExfatStreamEntry *Stream;
                CHAR AsciiName[256];

                if (ExfatParseFileEntryName(Ptr, End, AsciiName, sizeof(AsciiName))) {
                    Stream = ExfatFindStreamEntry(Ptr, File->SecondaryCount);
                    if (Stream && StrCmp(AsciiName, Name) == 0) {
                        VfsInode *Node = VfsAllocInode();
                        ExfatPrivate *NewPriv = (ExfatPrivate*)MemoryAllocate(sizeof(ExfatPrivate));

                        if (!Node || !NewPriv) {
                            if (Node)
                                VfsFreeInode(Node);
                            MemoryFree(NewPriv);
                            break;
                        }

                        MemSet(NewPriv, 0, sizeof(ExfatPrivate));
                        NewPriv->Exfat = Ctx;
                        NewPriv->FirstCluster = Stream->FirstCluster;
                        NewPriv->DataLength = Stream->DataLength;
                        NewPriv->DirCluster = Cluster;
                        NewPriv->DirEntry = (UINT32)((Ptr - Buf) / EXFAT_DIR_SLOT_SIZE);
                        NewPriv->ParentCluster = DirPriv->FirstCluster;
                        ExfatAttachParent(NewPriv, Dir);

                        Node->IMode = (File->FileAttributes & EXFAT_ATTR_DIRECTORY) ?
                                      FT_DIR : FT_REG_FILE;
                        Node->ISize = Stream->DataLength;
                        Node->IPrivate = NewPriv;
                        Node->IOp = Dir->IOp;
                        Node->IFop = Dir->IFop;
                        Node->IDev = Ctx->Disk;
                        StrCpy(Node->IFsName, "exfat");

                        *Result = Node;
                        Ret = 0;
                        break;
                    }
                }
            }

            Ptr = ExfatDirAdvance(Ptr, End, &DirEnded);
        }

        if (Ret == 0 || DirEnded) {
            break;
        }
        Cluster = ExfatNextCluster(Ctx, Cluster);
    }

    MemoryFree(Buf);
    return Ret;
}

STATIC INT ExfatCreate(VfsInode *Dir, const CHAR *Name, UINT32 Mode, VfsInode **Result) {
    ExfatPrivate *DirPriv;
    ExfatContext *Ctx;
    VfsInode *Existing;
    UINT32 ParentCluster;
    UINT32 FreeEntry;
    UINT8 *ParentBuf;
    UINT8 *Ptr;
    UINT8 SecondaryCount;
    UINT32 DataCluster;
    ExfatFileEntry *FileEntry;
    ExfatStreamEntry *Stream;
    ExfatNameEntry *NameEntry;
    UINT32 I;

    if (!Dir || !Name) {
        return -1;
    }
    if (Dir->IMode != FT_DIR) {
        return -1;
    }

    DirPriv = (ExfatPrivate*)Dir->IPrivate;
    if (!DirPriv || !DirPriv->Exfat) {
        return -1;
    }

    Ctx = DirPriv->Exfat;

    Existing = NULLPTR;
    if (ExfatLookup(Dir, Name, &Existing) == 0) {
        VfsInodeUnref(Existing);
        return -1;
    }

    DataCluster = 0;
    INT AllocRet = ExfatAllocCluster(Ctx, &DataCluster);
    if (AllocRet != 0) {
        return -1;
    }

    ParentBuf = NULLPTR;
    INT SlotRet = ExfatFindOrMakeDirSlot(DirPriv, Name, &ParentCluster, &FreeEntry, &ParentBuf);
    if (SlotRet != 0) {
        if (DataCluster >= 2) {
            Ctx->FatCache[DataCluster] = EXFAT_FAT_FREE;
            Ctx->FatCache[0] |= 1;
        }
        return -1;
    }

    Ptr = ParentBuf + FreeEntry * EXFAT_DIR_SLOT_SIZE;
    SecondaryCount = ExfatSecondaryCountForName(Name);

    MemSet(Ptr, 0, (USIZE)ExfatDirSlotsNeeded(Name) * EXFAT_DIR_SLOT_SIZE);

    FileEntry = (ExfatFileEntry*)Ptr;
    FileEntry->Type = EXFAT_ENTRY_FILE;
    FileEntry->SecondaryCount = SecondaryCount;
    FileEntry->FileAttributes = (Mode == FT_DIR) ? EXFAT_ATTR_DIRECTORY : 0;

    Stream = (ExfatStreamEntry*)(Ptr + EXFAT_DIR_SLOT_SIZE);
    Stream->Type = EXFAT_ENTRY_STREAM;
    Stream->NameLength = (UINT8)StrLen(Name);
    Stream->FirstCluster = DataCluster;
    Stream->DataLength = 0;
    Stream->ValidDataLength = 0;

    NameEntry = (ExfatNameEntry*)(Ptr + EXFAT_DIR_SLOT_SIZE * 2);
    NameEntry->Type = EXFAT_ENTRY_NAME;
    for (I = 0; I < StrLen(Name) && I < 15; I++) {
        NameEntry->Name[I] = Name[I];
    }

    FileEntry->Checksum = 0;
    FileEntry->Checksum = ExfatChecksumDirSet(Ptr, SecondaryCount);

    INT WriteRet = ExfatWriteCluster(Ctx, ParentCluster, ParentBuf);
    MemoryFree(ParentBuf);

    if (Result) {
        VfsInode *Inode = VfsAllocInode();
        ExfatPrivate *NewPriv = (ExfatPrivate*)MemoryAllocate(sizeof(ExfatPrivate));

        if (!Inode || !NewPriv) {
            if (Inode) {
                VfsFreeInode(Inode);
            }
            MemoryFree(NewPriv);
            return -1;
        }

        MemSet(NewPriv, 0, sizeof(ExfatPrivate));
        NewPriv->Exfat = Ctx;
        NewPriv->FirstCluster = DataCluster;
        NewPriv->DataLength = 0;
        NewPriv->DirCluster = ParentCluster;
        NewPriv->DirEntry = FreeEntry;
        NewPriv->ParentCluster = DirPriv->FirstCluster;
        ExfatAttachParent(NewPriv, Dir);

        Inode->IMode = Mode;
        Inode->ISize = 0;
        Inode->IPrivate = NewPriv;
        Inode->IOp = Dir->IOp;
        Inode->IFop = Dir->IFop;
        Inode->IDev = Ctx->Disk;

        *Result = Inode;
    }

    return 0;
}

STATIC INT ExfatUnlink(VfsInode *Dir, const CHAR *Name) {
    if (!Dir || !Name) return -1;
    if (Dir->IMode != FT_DIR) return -1;
    
    ExfatPrivate *DirPriv = (ExfatPrivate*)Dir->IPrivate;
    ExfatContext *Ctx = DirPriv->Exfat;
    
    // Find the file
    VfsInode *File = NULLPTR;
    if (ExfatLookup(Dir, Name, &File) != 0) return -1;
    
    ExfatPrivate *FilePriv = (ExfatPrivate*)File->IPrivate;

    UINT32 Cluster = FilePriv->FirstCluster;
    UINT32 ClusterSize = Ctx->BytesPerCluster;
    UINT8 *ZeroBuf = (UINT8*)MemoryAllocate(ClusterSize);
    
    if (ZeroBuf) {
        MemSet(ZeroBuf, 0, ClusterSize);
        while (Cluster >= 2 && Cluster != EXFAT_FAT_END) {
            ExfatWriteCluster(Ctx, Cluster, ZeroBuf);
            Cluster = ExfatNextCluster(Ctx, Cluster);
        }
        MemoryFree(ZeroBuf);
    }
    
    // Free clusters in FAT (empty files may have FirstCluster == 0)
    Cluster = FilePriv->FirstCluster;
    while (Cluster >= 2 && Cluster != EXFAT_FAT_END) {
        UINT32 Next = ExfatNextCluster(Ctx, Cluster);
        Ctx->FatCache[Cluster] = EXFAT_FAT_FREE;
        Cluster = Next;
    }
    
    // Mark entries as free
    UINT8 *Buf = (UINT8*)MemoryAllocate(Ctx->BytesPerCluster);
    UINT32 DirCluster = FilePriv->DirCluster ? FilePriv->DirCluster : DirPriv->FirstCluster;

    ExfatReadCluster(Ctx, DirCluster, Buf);

    UINT8 *Ptr = Buf + FilePriv->DirEntry * EXFAT_DIR_SLOT_SIZE;
    UINT8 Count = Ptr[1] + 1;

    for (UINT8 i = 0; i < Count; i++) {
        Ptr[i * EXFAT_DIR_SLOT_SIZE] = 0;
    }

    ExfatWriteCluster(Ctx, DirCluster, Buf);
    MemoryFree(Buf);
    
    Ctx->FatCache[0] |= 1;
    VfsInodeUnref(File);
    
    return 0;
}

STATIC INT ExfatMkdir(VfsInode *Dir, const CHAR *Name, UINT32 Mode, VfsInode **Result) {
    return ExfatCreate(Dir, Name, FT_DIR, Result);
}

STATIC INT ExfatRmdir(VfsInode *Dir, const CHAR *Name) {
    return ExfatUnlink(Dir, Name);
}

STATIC INT ExfatRename(VfsInode *OldDir, const CHAR *OldName, VfsInode *NewDir, const CHAR *NewName) {
    if (!OldDir || !OldName || !NewDir || !NewName) return -1;
    
    ExfatPrivate *OldPriv = (ExfatPrivate*)OldDir->IPrivate;
    ExfatContext *Ctx = OldPriv->Exfat;
    
    // Find the file
    VfsInode *File = NULLPTR;
    if (ExfatLookup(OldDir, OldName, &File) != 0) return -1;
    
    ExfatPrivate *FilePriv = (ExfatPrivate*)File->IPrivate;
    
    // Read old directory cluster
    UINT8 *OldBuf = (UINT8*)MemoryAllocate(Ctx->BytesPerCluster);
    ExfatReadCluster(Ctx, OldPriv->FirstCluster, OldBuf);
    
    UINT8 *OldPtr = OldBuf + FilePriv->DirEntry * 32;
    UINT8 Count = OldPtr[1] + 1;
    UINT8 *FileData = (UINT8*)MemoryAllocate(Count * 32);
    MemCpy(FileData, OldPtr, Count * 32);
    
    // Mark old entry as free
    for (UINT8 i = 0; i < Count; i++) {
        OldPtr[i * 32] = 0;
    }
    ExfatWriteCluster(Ctx, OldPriv->FirstCluster, OldBuf);
    
    // Find free entry in new directory
    ExfatPrivate *NewPriv = (ExfatPrivate*)NewDir->IPrivate;
    UINT8 *NewBuf = (UINT8*)MemoryAllocate(Ctx->BytesPerCluster);
    ExfatReadCluster(Ctx, NewPriv->FirstCluster, NewBuf);
    
    INT FreeEntry = -1;
    UINT8 *NewPtr = NewBuf;
    for (UINT32 i = 0; i < Ctx->BytesPerCluster / 32; i++) {
        if (NewPtr[0] == 0) {
            FreeEntry = i;
            break;
        }
        NewPtr += 32;
    }
    
    if (FreeEntry == -1) {
        // Restore old entry
        MemCpy(OldPtr, FileData, Count * 32);
        ExfatWriteCluster(Ctx, OldPriv->FirstCluster, OldBuf);
        MemoryFree(OldBuf);
        MemoryFree(NewBuf);
        MemoryFree(FileData);
        VfsInodeUnref(File);
        return -1;
    }
    
    // Copy file data to new location
    MemCpy(NewBuf + FreeEntry * 32, FileData, Count * 32);
    
    // Update name
    UINT8 *NamePtr = NewBuf + (FreeEntry + 2) * 32;
    ExfatNameEntry *NameEntry = (ExfatNameEntry*)NamePtr;
    MemSet(NameEntry->Name, 0, 30);
    for (UINT32 i = 0; i < StrLen(NewName) && i < 15; i++) {
        NameEntry->Name[i] = NewName[i];
    }
    
    ExfatWriteCluster(Ctx, NewPriv->FirstCluster, NewBuf);
    
    FilePriv->DirCluster = NewPriv->FirstCluster;
    FilePriv->DirEntry = FreeEntry;
    FilePriv->ParentCluster = NewPriv->FirstCluster;
    if (FilePriv->ParentInode != NewDir) {
        VfsInodeRef(NewDir);
        if (FilePriv->ParentInode)
            VfsInodeUnref(FilePriv->ParentInode);
        FilePriv->ParentInode = NewDir;
    }
    
    MemoryFree(OldBuf);
    MemoryFree(NewBuf);
    MemoryFree(FileData);
    VfsInodeUnref(File);
    
    return 0;
}

STATIC INT ExfatRead(VfsInode *Inode, UINT64 Offset, VOID *Buf, UINT32 Size, UINT32 *Read) {
    ExfatPrivate *Priv = (ExfatPrivate*)Inode->IPrivate;
    ExfatContext *Ctx = Priv->Exfat;
    
    if (Offset >= Inode->ISize) {
        *Read = 0;
        return 0;
    }
    
    UINT32 ToRead = Size;
    if (Offset + ToRead > Inode->ISize) {
        ToRead = Inode->ISize - Offset;
    }
    
    UINT8 *Buffer = (UINT8*)Buf;
    UINT32 Done = 0;
    UINT32 Cluster = Priv->FirstCluster;
    UINT32 ClusterSize = Ctx->BytesPerCluster;
    
    // Skip to offset cluster
    UINT32 SkipClusters = Offset / ClusterSize;
    for (UINT32 i = 0; i < SkipClusters && Cluster >= 2; i++) {
        Cluster = ExfatNextCluster(Ctx, Cluster);
    }
    
    if (Cluster < 2) {
        *Read = 0;
        return -1;
    }
    
    UINT32 ClusterOffset = Offset % ClusterSize;
    UINT8 *TempBuf = (UINT8*)MemoryAllocate(ClusterSize);
    
    while (Done < ToRead && Cluster >= 2 && Cluster != EXFAT_FAT_END) {
        ExfatReadCluster(Ctx, Cluster, TempBuf);
        
        UINT32 CopySize = ClusterSize - ClusterOffset;
        if (CopySize > ToRead - Done) CopySize = ToRead - Done;
        
        MemCpy(Buffer + Done, TempBuf + ClusterOffset, CopySize);
        
        Done += CopySize;
        ClusterOffset = 0;
        Cluster = ExfatNextCluster(Ctx, Cluster);
    }
    
    *Read = Done;
    MemoryFree(TempBuf);
    return 0;
}

INT ExfatSync(VfsInode *Inode);

STATIC INT ExfatWrite(VfsInode *Inode, UINT64 Offset, const VOID *Buf, UINT32 Size, UINT32 *Written) {
    ExfatPrivate *Priv = (ExfatPrivate*)Inode->IPrivate;
    ExfatContext *Ctx = Priv->Exfat;
    
    *Written = 0;
    if (Size == 0) return 0;
    
    UINT32 ClusterSize = Ctx->BytesPerCluster;
    const UINT8 *Buffer = (const UINT8*)Buf;
    UINT32 ToWrite = Size;
    UINT32 Done = 0;
    
    // Calculate needed clusters
    UINT32 FirstClusterIdx = Offset / ClusterSize;
    UINT32 LastClusterIdx = (Offset + Size - 1) / ClusterSize;
    UINT32 ClustersNeeded = LastClusterIdx - FirstClusterIdx + 1;
    
    UINT32 *Clusters = (UINT32*)MemoryAllocate(ClustersNeeded * sizeof(UINT32));
    if (!Clusters) return -1;
    
    // Navigate to offset cluster
    UINT32 Cluster = Priv->FirstCluster;
    for (UINT32 i = 0; i < FirstClusterIdx && Cluster >= 2; i++) {
        Cluster = ExfatNextCluster(Ctx, Cluster);
    }
    
    // Collect or create clusters
    UINT32 c = 0;
    while (c < ClustersNeeded) {
        if (Cluster < 2 || Cluster == EXFAT_FAT_END) {
            // Need new cluster
            UINT32 NewCluster = 0;
            for (UINT32 i = 2; i < Ctx->FatEntries; i++) {
                if (Ctx->FatCache[i] == EXFAT_FAT_FREE) {
                    NewCluster = i;
                    break;
                }
            }
            if (NewCluster == 0) {
                MemoryFree(Clusters);
                return -1;
            }
            
            if (c == 0 && FirstClusterIdx == 0) {
                Priv->FirstCluster = NewCluster;
            } else {
                Ctx->FatCache[Clusters[c - 1]] = NewCluster;
            }
            Ctx->FatCache[NewCluster] = EXFAT_FAT_END;
            Clusters[c] = NewCluster;
        } else {
            Clusters[c] = Cluster;
            Cluster = ExfatNextCluster(Ctx, Cluster);
        }
        c++;
    }
    
    // Write data
    for (UINT32 i = 0; i < ClustersNeeded; i++) {
        UINT32 CurrentCluster = Clusters[i];
        UINT32 ClusterOffset = (i == 0) ? (Offset % ClusterSize) : 0;
        UINT32 WriteSize = ClusterSize - ClusterOffset;
        if (WriteSize > ToWrite - Done) WriteSize = ToWrite - Done;
        
        if (WriteSize == ClusterSize) {
            ExfatWriteCluster(Ctx, CurrentCluster, Buffer + Done);
        } else {
            UINT8 *TempBuf = (UINT8*)MemoryAllocate(ClusterSize);
            ExfatReadCluster(Ctx, CurrentCluster, TempBuf);
            MemCpy(TempBuf + ClusterOffset, Buffer + Done, WriteSize);
            ExfatWriteCluster(Ctx, CurrentCluster, TempBuf);
            MemoryFree(TempBuf);
        }
        Done += WriteSize;
    }
    
    // Update directory stream entry (size and first cluster if allocated on write)
    {
        UINT8 *DirBuf = (UINT8*)MemoryAllocate(ClusterSize);
        UINT32 DirCluster = Priv->DirCluster ? Priv->DirCluster : Ctx->RootCluster;

        if (DirBuf && ExfatReadCluster(Ctx, DirCluster, DirBuf) == 0) {
            UINT8 *Ptr = DirBuf + Priv->DirEntry * EXFAT_DIR_SLOT_SIZE;
            ExfatStreamEntry *Stream = (ExfatStreamEntry*)(Ptr + EXFAT_DIR_SLOT_SIZE);

            Stream->FirstCluster = Priv->FirstCluster;
	    Stream->DataLength = Inode->ISize;
	    Stream->ValidDataLength = Inode->ISize;
            if (Offset + Done > Inode->ISize) {
                Inode->ISize = Offset + Done;
            }
            Priv->DataLength = Inode->ISize;
            Stream->DataLength = Inode->ISize;
            Stream->ValidDataLength = Inode->ISize;
            ExfatWriteCluster(Ctx, DirCluster, DirBuf);
        }
        if (DirBuf) {
            MemoryFree(DirBuf);
        }
    }
    
    MemoryFree(Clusters);
    *Written = Done;
    Ctx->FatCache[0] |= 1;
    ExfatSync(Inode);
    
    return 0;
}

STATIC INT ExfatTruncate(VfsInode *Inode, UINT64 NewSize) {
    ExfatPrivate *Priv = (ExfatPrivate*)Inode->IPrivate;
    ExfatContext *Ctx = Priv->Exfat;
    UINT32 ClusterSize = Ctx->BytesPerCluster;
    UINT32 Cluster = Priv->FirstCluster;
    UINT32 CurrentClusters = 0;
    UINT32 NeededClusters = (NewSize + ClusterSize - 1) / ClusterSize;
    UINT32 Temp;
    UINT32 LastCluster = 0;

    if (NewSize == 0) {
        // Freeing up all clusters
        Temp = Cluster;
        while (Temp >= 2 && Temp != EXFAT_FAT_END) {
            UINT32 Next = ExfatNextCluster(Ctx, Temp);
            Ctx->FatCache[Temp] = EXFAT_FAT_FREE;
            Temp = Next;
        }
        Priv->FirstCluster = 0;
        Inode->ISize = 0;
        Priv->DataLength = 0;
        return 0;
    }

    // Counting current clusters
    Temp = Cluster;
    while (Temp >= 2 && Temp != EXFAT_FAT_END) {
        CurrentClusters++;
        LastCluster = Temp;
        Temp = ExfatNextCluster(Ctx, Temp);
    }

    // If you need fewer clusters, we cut them down.
    if (CurrentClusters > NeededClusters) {
        Temp = Cluster;
        for (UINT32 i = 0; i < NeededClusters && Temp >= 2; i++) {
            LastCluster = Temp;
            Temp = ExfatNextCluster(Ctx, Temp);
        }
        // Freeing up the extra ones
        while (Temp >= 2 && Temp != EXFAT_FAT_END) {
            UINT32 Next = ExfatNextCluster(Ctx, Temp);
            Ctx->FatCache[Temp] = EXFAT_FAT_FREE;
            Temp = Next;
        }
        Ctx->FatCache[LastCluster] = EXFAT_FAT_END;
    }

    // If you need more clusters, select
    if (CurrentClusters < NeededClusters) {
        // Finding the last cluster
        if (CurrentClusters == 0) {
            if (ExfatAllocCluster(Ctx, &Cluster) != 0) return -1;
            Priv->FirstCluster = Cluster;
            LastCluster = Cluster;
            CurrentClusters = 1;
        } else {
            Temp = Cluster;
            while (Temp >= 2 && Temp != EXFAT_FAT_END) {
                LastCluster = Temp;
                Temp = ExfatNextCluster(Ctx, Temp);
            }
        }

        while (CurrentClusters < NeededClusters) {
            UINT32 NewCluster;
            if (ExfatAllocCluster(Ctx, &NewCluster) != 0) return -1;
            Ctx->FatCache[LastCluster] = NewCluster;
            Ctx->FatCache[NewCluster] = EXFAT_FAT_END;
            LastCluster = NewCluster;
            CurrentClusters++;
        }
    }

    Inode->ISize = NewSize;
    Priv->DataLength = NewSize;

    // Updating the catalog entry
    UINT8 *DirBuf = (UINT8*)MemoryAllocate(ClusterSize);
    if (DirBuf) {
        UINT32 DirCluster = Priv->DirCluster ? Priv->DirCluster : Ctx->RootCluster;
        if (ExfatReadCluster(Ctx, DirCluster, DirBuf) == 0) {
            UINT8 *Ptr = DirBuf + Priv->DirEntry * EXFAT_DIR_SLOT_SIZE;
            ExfatStreamEntry *Stream = (ExfatStreamEntry*)(Ptr + EXFAT_DIR_SLOT_SIZE);
            Stream->FirstCluster = Priv->FirstCluster;
            Stream->DataLength = NewSize;
            Stream->ValidDataLength = NewSize;
            ExfatWriteCluster(Ctx, DirCluster, DirBuf);
        }
        MemoryFree(DirBuf);
    }

    return 0;
}

INT ExfatSync(VfsInode *Inode) {
    ExfatPrivate *Priv = (ExfatPrivate*)Inode->IPrivate;
    ExfatContext *Ctx = Priv->Exfat;
    
    if (Ctx->FatCache[0] & 1) {
        for (UINT32 i = 0; i < Ctx->Vbr.FatLength; i++) {
            UINT64 Sector = Ctx->Vbr.FatOffset + i;
            ExfatWriteSectors(Ctx, Sector, 1, (UINT8*)Ctx->FatCache + i * Ctx->BytesPerSector);
        }
        Ctx->FatCache[0] &= ~1;
    }
    return 0;
}

STATIC INT ExfatChmod(VfsInode *Inode, UINT32 Mode) {
    if (!Inode) return -1;
    
    ExfatPrivate *Priv = (ExfatPrivate*)Inode->IPrivate;
    ExfatContext *Ctx = Priv->Exfat;
    
    UINT16 Attributes = 0;
    if (Inode->IMode == FT_DIR) {
        Attributes |= EXFAT_ATTR_DIRECTORY;
    }
    if (!(Mode & 0200)) {
        Attributes |= EXFAT_ATTR_READ_ONLY;
    }
    
    UINT8 *Buf = (UINT8*)MemoryAllocate(Ctx->BytesPerCluster);
    ExfatReadCluster(Ctx, Priv->DirCluster, Buf);
    
    UINT8 *Ptr = Buf + Priv->DirEntry * 32;
    ExfatFileEntry *FileEntry = (ExfatFileEntry*)Ptr;
    FileEntry->FileAttributes = Attributes;
    
    ExfatWriteCluster(Ctx, Priv->DirCluster, Buf);
    MemoryFree(Buf);
    
    Inode->IMode = Mode;
    return 0;
}

STATIC INT ExfatStat(VfsInode *Inode, VOID *StatBuf) {
    return VfsStat(Inode, (VfsStats*)StatBuf);
}

STATIC INT ExfatReadDir(VfsInode *Dir, UINT64 *Pos, CHAR *Name, UINT32 *NameLen, UINT32 *Type) {
    ExfatPrivate *Priv;
    ExfatContext *Ctx;
    UINT32 Cluster;
    UINT32 ChainIters;
    UINT8 *Buf;
    UINT8 *Ptr;
    UINT8 *End;
    UINT64 Index;
    UINT64 Target;
    BOOL DirEnded;

    if (!Dir || !Pos || !Name || !NameLen || !Type) {
        return -1;
    }
    if (Dir->IMode != FT_DIR) {
        return -1;
    }

    Priv = (ExfatPrivate*)Dir->IPrivate;
    if (!Priv || !Priv->Exfat) {
        return -1;
    }

    Ctx = Priv->Exfat;
    Target = *Pos;
    Index = 0;
    Cluster = Priv->FirstCluster;
    ChainIters = 0;

    Buf = (UINT8*)MemoryAllocate(Ctx->BytesPerCluster);
    if (!Buf) {
        return -1;
    }

    while (Cluster >= 2 && Cluster != EXFAT_FAT_END &&
           ChainIters++ < EXFAT_MAX_DIR_CHAIN_ITER) {
        if (ExfatReadCluster(Ctx, Cluster, Buf) != 0) {
            break;
        }

        Ptr = Buf;
        End = Buf + Ctx->BytesPerCluster;
        DirEnded = FALSE;

        while (Ptr < End && !DirEnded) {
            UINT8 EntryType = Ptr[0];

            if (EntryType == EXFAT_ENTRY_FILE) {
                ExfatFileEntry *FileEntry = (ExfatFileEntry*)Ptr;
                INT NamePos = 0;

                if (ExfatParseFileEntryName(Ptr, End, Name, 256)) {
                    NamePos = StrLen(Name);
                    if (Index == Target) {
                        *NameLen = (UINT32)NamePos;
                        *Type = (FileEntry->FileAttributes & EXFAT_ATTR_DIRECTORY) ?
                                FT_DIR : FT_REG_FILE;
                        (*Pos)++;
                        MemoryFree(Buf);
                        return 0;
                    }
                    Index++;
                }
            }

            Ptr = ExfatDirAdvance(Ptr, End, &DirEnded);
        }

        if (DirEnded) {
            break;
        }
        Cluster = ExfatNextCluster(Ctx, Cluster);
    }

    MemoryFree(Buf);
    return -1;
}

STATIC INT ExfatParent(VfsInode *Inode, VfsInode **Parent) {
    ExfatPrivate *Priv;

    if (!Inode || !Parent) {
        return -1;
    }

    Priv = (ExfatPrivate *)Inode->IPrivate;
    if (!Priv) {
        return -1;
    }

    if (!Priv->ParentInode) {
        return -1;
    }

    *Parent = Priv->ParentInode;
    VfsInodeRef(*Parent);
    return 0;
}

STATIC INT ExfatGetName(VfsInode *Inode, CHAR *Name, INT MaxLen) {
    ExfatPrivate *Priv;
    ExfatContext *Ctx;
    UINT8 *Buf;
    UINT8 *Ptr;
    ExfatFileEntry *File;
    UINT8 Count;
    UINT8 *NamePtr;
    INT NamePos;
    UINT8 I;
    INT J;

    if (!Inode || !Name || MaxLen <= 0) {
        return -1;
    }

    Priv = (ExfatPrivate*)Inode->IPrivate;
    if (!Priv || !Priv->Exfat) {
        return -1;
    }

    Ctx = Priv->Exfat;

    if (Inode == Ctx->MountRoot || Priv->FirstCluster == Ctx->RootCluster) {
        Name[0] = '\0';
        return 0;
    }

    if (Priv->DirCluster == 0) {
        return -1;
    }

    Buf = (UINT8*)MemoryAllocate(Ctx->BytesPerCluster);
    if (!Buf) {
        return -1;
    }
    if (ExfatReadCluster(Ctx, Priv->DirCluster, Buf) != 0) {
        MemoryFree(Buf);
        return -1;
    }

    Ptr = Buf + Priv->DirEntry * EXFAT_DIR_SLOT_SIZE;
    
    if (Ptr[0] != EXFAT_ENTRY_FILE) {
        MemoryFree(Buf);
        return -1;
    }

    File = (ExfatFileEntry*)Ptr;
    Count = File->SecondaryCount;
    NamePtr = Ptr + EXFAT_DIR_SLOT_SIZE;
    NamePos = 0;

    for (I = 0; I < Count; I++) {
        if (NamePtr[0] == EXFAT_ENTRY_STREAM) {
            NamePtr += EXFAT_DIR_SLOT_SIZE;
            break;
        }
        NamePtr += EXFAT_DIR_SLOT_SIZE;
    }

    for (I = 0; I < Count - 1 && NamePos < MaxLen - 1; I++) {
        if (NamePtr[0] == EXFAT_ENTRY_NAME) {
            ExfatNameEntry *NameEntry = (ExfatNameEntry*)NamePtr;
            for (J = 0; J < 15; J++) {
                UINT16 C = NameEntry->Name[J];
                if (C == 0) {
                    break;
                }
                if (C < 128) {
                    Name[NamePos++] = (CHAR)C;
                }
            }
        }
        NamePtr += EXFAT_DIR_SLOT_SIZE;
    }

    Name[NamePos] = '\0';
    MemoryFree(Buf);
    return 0;
}

STATIC INT ExfatUnmount(VfsInode *Root) {
    ExfatPrivate *Priv;
    ExfatContext *Ctx;

    if (!Root)
        return -1;

    Priv = (ExfatPrivate *)Root->IPrivate;
    if (!Priv || !Priv->Exfat)
        return -1;

    Ctx = Priv->Exfat;
    if (Ctx->FatCache && (Ctx->FatCache[0] & 1)) {
        for (UINT32 i = 0; i < Ctx->Vbr.FatLength; i++) {
            UINT64 Sector = Ctx->Vbr.FatOffset + i;
            ExfatWriteSectors(Ctx, Sector, 1, (UINT8 *)Ctx->FatCache + i * Ctx->BytesPerSector);
        }
    }

    return 0;
}

/* ============================================================================
 * VFS Operations Tables
 * ============================================================================ */

STATIC VfsOperations GExfatNodeOps = {
    .Lookup = ExfatLookup,
    .Create = ExfatCreate,
    .Unlink = ExfatUnlink,
    .Mkdir = ExfatMkdir,
    .Rmdir = ExfatRmdir,
    .Rename = ExfatRename,
    .Chmod = ExfatChmod,
    .Stat = ExfatStat,
    .ReadDir = ExfatReadDir,
    .Parent = ExfatParent,
    .GetName = ExfatGetName,
    .Unmount = ExfatUnmount,
    .Destroy = ExfatDestroy
};

STATIC VfsFileOperations GExfatFileOps = {
    .Read = ExfatRead,
    .Write = ExfatWrite,
    .Truncate = ExfatTruncate,
    .Sync = ExfatSync
};

/* ============================================================================
 * Mount / Unmount
 * ============================================================================ */
STATIC INT ExfatMount(Disk *Disk, UINT64 StartLba, UINT64 SectorCount, VfsInode **Root) {
    ExfatContext *Ctx;
    ExfatVbr *Vbr;
    UINT8 Sector[512];
    UINT32 FatSizeBytes;
    
    if (!Disk || !Root) return -1;
    
    Ctx = (ExfatContext*)MemoryAllocate(sizeof(ExfatContext));
    if (!Ctx) return -1;
    
    MemSet(Ctx, 0, sizeof(ExfatContext));
    Ctx->Disk = Disk;
    Ctx->StartLba = StartLba;
    Ctx->SectorCount = SectorCount ? SectorCount : Disk->SectorCount;
    
    if (Disk->Read(Disk, StartLba, 1, Sector) != 0) {
        MemoryFree(Ctx);
        return -1;
    }
    
    Vbr = (ExfatVbr*)Sector;
    
    if (MemCmp(Vbr->FsName, "EXFAT   ", 8) != 0) {
        MemoryFree(Ctx);
        return -1;
    }
    
    Ctx->Vbr = *Vbr;
    Ctx->BytesPerSector = 1 << Vbr->BytesPerSectorShift;
    Ctx->SectorsPerCluster = 1 << Vbr->SectorsPerClusterShift;
    Ctx->BytesPerCluster = Ctx->BytesPerSector * Ctx->SectorsPerCluster;
    Ctx->FatStart = (UINT64)Vbr->FatOffset * Ctx->BytesPerSector;
    Ctx->ClusterHeapStart = (UINT64)Vbr->ClusterHeapOffset * Ctx->BytesPerSector;
    Ctx->RootCluster = Vbr->RootDirCluster;
    
    FatSizeBytes = Vbr->FatLength * Ctx->BytesPerSector;
    Ctx->FatEntries = FatSizeBytes / 4;
    Ctx->FatCache = (UINT32*)MemoryAllocate(FatSizeBytes);
    
    if (!Ctx->FatCache) {
        MemoryFree(Ctx);
        return -1;
    }
    
    for (UINT32 i = 0; i < Vbr->FatLength; i++) {
        UINT64 SectorNum = Vbr->FatOffset + i;
        Disk->Read(Disk, StartLba + SectorNum, 1,
                    (UINT8*)Ctx->FatCache + i * Ctx->BytesPerSector);
    }
    
    VfsInode *RootNode = VfsAllocInode();
    if (!RootNode) {
        MemoryFree(Ctx->FatCache);
        MemoryFree(Ctx);
        return -1;
    }
    
    ExfatPrivate *Priv = (ExfatPrivate*)MemoryAllocate(sizeof(ExfatPrivate));
    if (!Priv) {
        VfsFreeInode(RootNode);
        MemoryFree(Ctx->FatCache);
        MemoryFree(Ctx);
        return -1;
    }
    
    Priv->Exfat = Ctx;
    Priv->FirstCluster = Ctx->RootCluster;
    Priv->DataLength = 0;
    Priv->DirCluster = Ctx->RootCluster;
    Priv->DirEntry = 0;
    Priv->ParentCluster = Ctx->RootCluster;
    
    RootNode->IMode = FT_DIR;
    RootNode->ISize = 0;
    RootNode->IPrivate = Priv;
    RootNode->IOp = &GExfatNodeOps;
    RootNode->IFop = &GExfatFileOps;
    RootNode->IDev = Disk;
    StrCpy(RootNode->IFsName, "exfat");

    Ctx->MountRoot = RootNode;

    *Root = RootNode;
    return 0;
}

/* ============================================================================
 * File System Registration
 * ============================================================================ */

STATIC FileSystem GExfatFs = {
    .Name = "exfat",
    .Mount = ExfatMount,
    .Unmount = ExfatUnmount,
    .Next = NULLPTR
};

INT ExfatInit(VOID) {
    VfsRegisterFs(&GExfatFs);
    return 0;
}

/* ============================================================================
 * Formatting
 * ============================================================================ */

INT ExfatFormat(Disk *Disk, UINT64 StartLba, UINT64 SectorCount) {
    ExfatVbr Vbr;
    UINT32 *Fat;
    UINT32 SectorSize;
    UINT64 TotalSectors;
    UINT8 SectorsPerClusterShift;
    UINT32 SectorsPerCluster;
    UINT32 ClusterSize;
    UINT64 TotalClusters;
    UINT32 FatEntries;
    UINT32 FatSectors;
    UINT32 FatOffset;
    UINT32 ClusterHeapOffset;
    UINT32 RootCluster;
    UINT8 *ClusterBuf;
    UINT64 RootSector;

    if (!Disk || !Disk->Write) return -1;

    SectorSize = Disk->SectorSize;

    // If SectorCount == 0, format the entire disk.
    if (SectorCount == 0) {
        TotalSectors = Disk->SectorCount;
        StartLba = 0;
    } else {
        TotalSectors = SectorCount;
    }

    if (TotalSectors < 0x100000) {
        SectorsPerClusterShift = 0;
    } else if (TotalSectors < 0x400000) {
        SectorsPerClusterShift = 1;
    } else if (TotalSectors < 0x1000000) {
        SectorsPerClusterShift = 3;
    } else {
        SectorsPerClusterShift = 6;
    }

    SectorsPerCluster = 1 << SectorsPerClusterShift;
    ClusterSize = SectorSize * SectorsPerCluster;

    TotalClusters = TotalSectors / SectorsPerCluster;
    FatEntries = TotalClusters + 2;
    FatSectors = (FatEntries * 4 + SectorSize - 1) / SectorSize;

    FatOffset = 24;
    ClusterHeapOffset = FatOffset + FatSectors;
    RootCluster = 2;

    // Create VBR
    MemSet(&Vbr, 0, sizeof(ExfatVbr));

    Vbr.JumpBoot[0] = 0xEB;
    Vbr.JumpBoot[1] = 0x76;
    Vbr.JumpBoot[2] = 0x90;
    MemCpy(Vbr.FsName, "EXFAT   ", 8);
    Vbr.PartitionOffset = StartLba;        // ← LBA of the beginning of the partition
    Vbr.VolumeLength = TotalSectors;
    Vbr.FatOffset = FatOffset;
    Vbr.FatLength = FatSectors;
    Vbr.ClusterHeapOffset = ClusterHeapOffset;
    Vbr.ClusterCount = TotalClusters;
    Vbr.RootDirCluster = RootCluster;
    Vbr.VolumeSerial = 0x12345678;
    Vbr.FsRevision = 0x0100;
    Vbr.VolumeFlags = 0;
    Vbr.BytesPerSectorShift = 9;
    Vbr.SectorsPerClusterShift = SectorsPerClusterShift;
    Vbr.NumberOfFats = 1;
    Vbr.DiskSelect = 0x80;
    Vbr.PercentInUse = 0;
    Vbr.BootCode[0] = 0xF4;
    Vbr.Signature = 0xAA55;

    // We write everything with the StartLba offset.
    if (Disk->Write(Disk, StartLba + 0, 1, &Vbr) != 0) return -1;

    // Create FAT
    Fat = (UINT32*)MemoryAllocate(FatSectors * SectorSize);
    if (!Fat) return -1;

    MemSet(Fat, 0, FatSectors * SectorSize);
    Fat[0] = 0xFFFFFFF8;
    Fat[1] = 0xFFFFFFFF;
    Fat[RootCluster] = 0xFFFFFFFF;

    UINT32 BlockSize = 256;
    UINT8 *BlockBuf = (UINT8*)MemoryAllocate(BlockSize * SectorSize);
    if (!BlockBuf) {
        MemoryFree(Fat);
        return -1;
    }

    UINT32 LastPercent = 0xFFFFFFFF;
    BaseconPrintf(BASECON_TYPE_NORMAL, "[");

    for (UINT32 I = 0; I < FatSectors; I += BlockSize) {
        UINT32 Chunk = (I + BlockSize > FatSectors) ? FatSectors - I : BlockSize;

        for (UINT32 K = 0; K < Chunk; K++) {
            MemCpy(BlockBuf + K * SectorSize,
                   (UINT8*)Fat + (I + K) * SectorSize,
                   SectorSize);
        }

        // StartLba Offset
        if (Disk->Write(Disk, StartLba + FatOffset + I, Chunk, BlockBuf) != 0) {
            MemoryFree(BlockBuf);
            MemoryFree(Fat);
            return -1;
        }

        UINT32 Percent = ((I + Chunk) * 100) / FatSectors;
        if (Percent != LastPercent) {
            UINT32 Pos = (Percent * 50) / 100;

            BaseconPrintf(BASECON_TYPE_NORMAL, "\r[");
            for (UINT32 J = 0; J < 50; J++) {
                BaseconPrintf(BASECON_TYPE_NORMAL, J < Pos ? "=" : (J == Pos ? ">" : " "));
            }
            BaseconPrintf(BASECON_TYPE_NORMAL, "] %3u%%", Percent);
            LastPercent = Percent;
        }
    }

    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    MemoryFree(BlockBuf);
    MemoryFree(Fat);

    // Initialize root directory
    ClusterBuf = (UINT8*)MemoryAllocate(ClusterSize);
    if (!ClusterBuf) return -1;

    MemSet(ClusterBuf, 0, ClusterSize);

    RootSector = ClusterHeapOffset + (RootCluster - 2) * SectorsPerCluster;

    // StartLba Offset
    if (Disk->Write(Disk, StartLba + RootSector, SectorsPerCluster, ClusterBuf) != 0) {
        MemoryFree(ClusterBuf);
        return -1;
    }
    MemoryFree(ClusterBuf);

    if (Disk->Flush) Disk->Flush(Disk);

    INT CfgRc = DiskCfgWriteVolume(Disk, StartLba,
                                    SectorCount ? SectorCount : Disk->SectorCount,
                                    DISKCFG_FS_EXFAT, "", FALSE);
    if (CfgRc != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_WARNING,
                      "fat32: disk.cfg not written (%d)\n", CfgRc);
    }

    return 0;
}