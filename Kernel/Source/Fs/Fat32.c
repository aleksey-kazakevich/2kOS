#include <Drivers/Disk/DiskMgr.h>
#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Time/Rtc.h>
#include <Basecon.h>
#include <Fs/Fat32.h>
#include <Return.h>

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

STATIC INT Fat32ReadSectors(Fat32 *Fat, UINT64 Sector, UINT32 Count, VOID *Buf) {
    return Fat->Dev->Read(Fat->Dev, Sector, Count, Buf);
}

STATIC INT Fat32WriteSectors(Fat32 *Fat, UINT64 Sector, UINT32 Count, const VOID *Buf) {
    return Fat->Dev->Write(Fat->Dev, Sector, Count, Buf);
}

STATIC UINT32 Fat32NextCluster(Fat32 *Fat, UINT32 Cluster) {
    if (Cluster < 2 || Cluster >= Fat->FatEntries) return FAT32_EOC;
    UINT32 Next = Fat->FatCache[Cluster];
    if (Next >= FAT32_EOC) return FAT32_EOC;
    if (Next == FAT32_FREE || Next == FAT32_BAD) return FAT32_EOC;
    return Next;
}

STATIC INT Fat32ReadCluster(Fat32 *Fat, UINT32 Cluster, UINT8 *Buffer) {
    if (Cluster < 2) return -1;
    UINT64 Sector = Fat->DataStart + (UINT64)(Cluster - 2) * Fat->SectorsPerCluster;
    return Fat32ReadSectors(Fat, Sector, Fat->SectorsPerCluster, Buffer);
}

STATIC INT Fat32WriteCluster(Fat32 *Fat, UINT32 Cluster, const UINT8 *Buffer) {
    if (Cluster < 2) return -1;
    UINT64 Sector = Fat->DataStart + (UINT64)(Cluster - 2) * Fat->SectorsPerCluster;
    return Fat32WriteSectors(Fat, Sector, Fat->SectorsPerCluster, Buffer);
}

STATIC UINT32 Fat32AllocCluster(Fat32 *Fat) {
    UINT32 Start = Fat->FsInfo.NextFree;
    if (Start < 2 || Start >= Fat->FatEntries) Start = 2;

    for (UINT32 I = 0; I < Fat->FatEntries - 2; I++) {
        UINT32 Cluster = Start + I;
        if (Cluster >= Fat->FatEntries) Cluster = 2 + (Cluster - Fat->FatEntries);

        if (Fat->FatCache[Cluster] == FAT32_FREE) {
            Fat->FatCache[Cluster] = FAT32_EOC;
            Fat->FsInfo.FreeCount--;
            Fat->FsInfo.NextFree = Cluster + 1;
            if (Fat->FsInfo.NextFree >= Fat->FatEntries) Fat->FsInfo.NextFree = 2;
            Fat->Dirty = 1;
            return Cluster;
        }
    }
    return 0;
}

STATIC VOID Fat32FreeClusterChain(Fat32 *Fat, UINT32 Cluster) {
    while (Cluster >= 2 && Cluster < Fat->FatEntries) {
        UINT32 Next = Fat->FatCache[Cluster];
        Fat->FatCache[Cluster] = FAT32_FREE;
        Fat->FsInfo.FreeCount++;
        Cluster = (Next < FAT32_EOC) ? Next : 0;
        Fat->Dirty = 1;
    }
}

STATIC INT Fat32AllocClusters(Fat32 *Fat, UINT32 Count, UINT32 *First) {
    *First = 0;
    UINT32 Prev = 0;

    for (UINT32 I = 0; I < Count; I++) {
        UINT32 Cluster = Fat32AllocCluster(Fat);
        if (!Cluster) {
            if (*First) Fat32FreeClusterChain(Fat, *First);
            return 0;
        }

        if (!*First) *First = Cluster;
        if (Prev) Fat->FatCache[Prev] = Cluster;
        Prev = Cluster;
    }
    return Count;
}

STATIC VOID Fat32UpdateFsInfo(Fat32 *Fat) {
    if (!Fat->Dirty) return;

    UINT8 Sector[512];
    MemSet(Sector, 0, 512);

    Fat32FsInfo *FsInfo = (Fat32FsInfo*)Sector;
    FsInfo->LeadSignature   = 0x41615252;
    FsInfo->StructSignature = 0x61417272;
    FsInfo->FreeCount       = Fat->FsInfo.FreeCount;
    FsInfo->NextFree        = Fat->FsInfo.NextFree;
    FsInfo->TrailSignature  = 0xAA550000;

    Fat32WriteSectors(Fat, Fat->FsinfoSector, 1, Sector);
    Fat->Dirty = 0;
}

STATIC UINT8 Fat32Checksum(const UINT8 *Name) {
    UINT8 Sum = 0;
    for (INT I = 0; I < 11; I++) {
        Sum = ((Sum & 1) ? 0x80 : 0) + (Sum >> 1) + Name[I];
    }
    return Sum;
}

STATIC VOID Fat32NameFromEntry(CHAR *Dest, const UINT8 *Name) {
    INT I, J = 0;

    for (I = 0; I < 8 && Name[I] != ' '; I++) {
        CHAR C = Name[I];
        Dest[J++] = (C >= 'A' && C <= 'Z') ? C + 32 : C;
    }

    if (Name[8] != ' ') {
        Dest[J++] = '.';
        for (I = 8; I < 11 && Name[I] != ' '; I++) {
            CHAR C = Name[I];
            Dest[J++] = (C >= 'A' && C <= 'Z') ? C + 32 : C;
        }
    }

    Dest[J] = '\0';
}

STATIC VOID Fat32NameToEntry(UINT8 *Dest, const CHAR *Name, UINT8 *Checksum) {
    for (INT I = 0; I < 11; I++) Dest[I] = ' ';

    INT I = 0;
    INT J = 0;

    while (Name[I] && Name[I] != '.' && I < 8) {
        CHAR C = Name[I];
        Dest[J++] = (C >= 'a' && C <= 'z') ? C - 32 : C;
        I++;
    }

    if (Name[I] == '.') I++;

    J = 8;
    while (Name[I] && J < 11) {
        CHAR C = Name[I];
        Dest[J++] = (C >= 'a' && C <= 'z') ? C - 32 : C;
        I++;
    }

    if (Checksum) *Checksum = Fat32Checksum(Dest);
}

STATIC INT Fat32CreateLfnEntries(Fat32 *Fat, UINT8 *ClusterBuf, INT *Offset,
                                 const CHAR *Name, UINT8 Checksum, INT IsLast) {
    (VOID)Fat;
    USIZE NameLen = StrLen(Name);
    INT NumEntries = (NameLen + 12) / 13;

    for (INT I = NumEntries - 1; I >= 0; I--) {
        Fat32LfnEntry *Lfn = (Fat32LfnEntry*)(ClusterBuf + *Offset);
        MemSet(Lfn, 0, sizeof(Fat32LfnEntry));

        Lfn->Order = (I + 1) | (IsLast && I == NumEntries - 1 ? 0x40 : 0);
        Lfn->Attr  = FAT_ATTR_LFN;
        Lfn->Type  = 0;
        Lfn->Checksum = Checksum;
        Lfn->FirstCluster = 0;

        INT NamePos = I * 13;
        for (INT J = 0; J < 5 && NamePos < (INT)NameLen; J++, NamePos++)
            Lfn->Name1[J] = Name[NamePos];
        for (INT J = 0; J < 6 && NamePos < (INT)NameLen; J++, NamePos++)
            Lfn->Name2[J] = Name[NamePos];
        for (INT J = 0; J < 2 && NamePos < (INT)NameLen; J++, NamePos++)
            Lfn->Name3[J] = Name[NamePos];

        *Offset += 32;
    }

    return NumEntries;
}

// ============================================================================
// VFS OPERATIONS
// ============================================================================

STATIC INT Fat32Lookup(VfsInode *Dir, const CHAR *Name, VfsInode **Result) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Dir->IPrivate;
    Fat32 *Fat = Priv->Fat;

    UINT32 Cluster = Priv->FirstCluster;
    UINT8 *Buf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
    if (!Buf) return -1;
    UINT32 EntryIndex = 0;

    while (Cluster && Cluster < FAT32_EOC) {
        if (Fat32ReadCluster(Fat, Cluster, Buf) != 0) {
            MemoryFree(Buf);
            return -1;
        }

        Fat32DirEntry *Entry = (Fat32DirEntry*)Buf;
        for (UINT32 I = 0; I < Fat->BytesPerCluster / 32; I++, EntryIndex++) {
            if (Entry->Name[0] == 0) {
                MemoryFree(Buf);
                return -1;
            }
            if (Entry->Name[0] == 0xE5) {
                Entry++;
                continue;
            }
            if (Entry->Attr == FAT_ATTR_LFN) {
                Entry++;
                continue;
            }

            CHAR EntryName[256];
            Fat32NameFromEntry(EntryName, Entry->Name);

            if (StrCmp(EntryName, Name) == 0) {
                VfsInode *Inode = VfsAllocInode();
                Fat32InodePrivate *NewPriv =
                    (Fat32InodePrivate*)MemoryAllocate(sizeof(Fat32InodePrivate));

                UINT32 FirstCluster = (Entry->FirstClusterHi << 16) | Entry->FirstClusterLo;

                NewPriv->Fat = Fat;
                NewPriv->FirstCluster = FirstCluster;
                NewPriv->DirCluster = Cluster;
                NewPriv->DirEntry = EntryIndex;
                NewPriv->EntryOffset = I * 32;
                NewPriv->ParentCluster = Priv->FirstCluster;

                Inode->IMode = (Entry->Attr & FAT_ATTR_DIRECTORY) ? FT_DIR : FT_REG_FILE;
                Inode->ISize = Entry->FileSize;
                Inode->IPrivate = NewPriv;
                Inode->IOp = Dir->IOp;
                Inode->IFop = Dir->IFop;
                Inode->IDev = Fat->Dev;
                StrCpy(Inode->IFsName, "fat32");

                *Result = Inode;
                MemoryFree(Buf);
                return 0;
            }

            Entry++;
        }

        Cluster = Fat32NextCluster(Fat, Cluster);
    }

    MemoryFree(Buf);
    return -1;
}

STATIC INT Fat32Read(VfsInode *Inode, UINT64 Offset, VOID *Buf, UINT32 Size, UINT32 *Read) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Inode->IPrivate;
    Fat32 *Fat = Priv->Fat;

    if (Offset >= Inode->ISize) {
        *Read = 0;
        return 0;
    }

    UINT32 ToRead = Size;
    if (Offset + ToRead > Inode->ISize)
        ToRead = (UINT32)(Inode->ISize - Offset);

    UINT8 *Buffer = (UINT8*)Buf;
    UINT32 Done = 0;
    UINT32 Cluster = Priv->FirstCluster;
    UINT32 ClusterSize = Fat->BytesPerCluster;

    UINT32 SkipClusters = (UINT32)(Offset / ClusterSize);
    for (UINT32 I = 0; I < SkipClusters && Cluster && Cluster < FAT32_EOC; I++)
        Cluster = Fat32NextCluster(Fat, Cluster);

    if (!Cluster || Cluster >= FAT32_EOC) {
        *Read = 0;
        return -1;
    }

    UINT32 ClusterOffset = (UINT32)(Offset % ClusterSize);
    UINT8 *Temp = (UINT8*)MemoryAllocate(ClusterSize);
    if (!Temp) return -1;

    while (Done < ToRead && Cluster && Cluster < FAT32_EOC) {
        if (Fat32ReadCluster(Fat, Cluster, Temp) != 0) {
            MemoryFree(Temp);
            return -1;
        }

        UINT32 Copy = ClusterSize - ClusterOffset;
        if (Copy > ToRead - Done) Copy = ToRead - Done;

        MemCpy(Buffer + Done, Temp + ClusterOffset, Copy);

        Done += Copy;
        ClusterOffset = 0;
        Cluster = Fat32NextCluster(Fat, Cluster);
    }

    MemoryFree(Temp);
    *Read = Done;
    return 0;
}

STATIC INT Fat32Write(VfsInode *Inode, UINT64 Offset, const VOID *Buf,
                      UINT32 Size, UINT32 *Written) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Inode->IPrivate;
    Fat32 *Fat = Priv->Fat;

    if (Size == 0) {
        *Written = 0;
        return 0;
    }

    UINT32 ClusterSize = Fat->BytesPerCluster;
    const UINT8 *Buffer = (const UINT8*)Buf;
    UINT32 ToWrite = Size;
    UINT32 Done = 0;

    UINT32 FirstClusterIdx = (UINT32)(Offset / ClusterSize);
    UINT32 LastClusterIdx  = (UINT32)((Offset + Size - 1) / ClusterSize);
    UINT32 ClustersNeeded  = LastClusterIdx - FirstClusterIdx + 1;

    UINT32 Cluster = Priv->FirstCluster;

    for (UINT32 I = 0; I < FirstClusterIdx; I++) {
        if (!Cluster || Cluster >= FAT32_EOC) {
            if (!Priv->FirstCluster) {
                if (!Fat32AllocClusters(Fat, 1, &Priv->FirstCluster))
                    return -1;
                Cluster = Priv->FirstCluster;
            } else {
                return -1;
            }
        } else {
            Cluster = Fat32NextCluster(Fat, Cluster);
        }
    }

    UINT32 *Clusters = (UINT32*)MemoryAllocate(ClustersNeeded * sizeof(UINT32));
    if (!Clusters) return -1;

    UINT32 C = 0;
    while (C < ClustersNeeded) {
        if (!Cluster || Cluster >= FAT32_EOC) {
            UINT32 NewCluster = Fat32AllocCluster(Fat);
            if (!NewCluster) {
                MemoryFree(Clusters);
                return -1;
            }

            if (C == 0 && !Priv->FirstCluster) {
                Priv->FirstCluster = NewCluster;
            } else if (C > 0) {
                Fat->FatCache[Clusters[C-1]] = NewCluster;
            }

            Clusters[C] = NewCluster;
            Cluster = NewCluster;
        } else {
            Clusters[C] = Cluster;
            Cluster = Fat32NextCluster(Fat, Cluster);
        }
        C++;
    }

    for (UINT32 I = 0; I < ClustersNeeded; I++) {
        UINT32 Current = Clusters[I];
        UINT32 ClusterOffset = (I == 0) ? (UINT32)(Offset % ClusterSize) : 0;
        UINT32 WriteSize = ClusterSize - ClusterOffset;
        if (WriteSize > ToWrite - Done) WriteSize = ToWrite - Done;

        if (WriteSize == ClusterSize) {
            Fat32WriteCluster(Fat, Current, Buffer + Done);
        } else {
            UINT8 *Temp = (UINT8*)MemoryAllocate(ClusterSize);
            if (!Temp) {
                MemoryFree(Clusters);
                return -1;
            }
            Fat32ReadCluster(Fat, Current, Temp);
            MemCpy(Temp + ClusterOffset, Buffer + Done, WriteSize);
            Fat32WriteCluster(Fat, Current, Temp);
            MemoryFree(Temp);
        }

        Done += WriteSize;
    }

    MemoryFree(Clusters);

    if (Offset + Done > Inode->ISize) {
        Inode->ISize = Offset + Done;

        UINT8 *DirBuf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
        if (!DirBuf) return -1;

        Fat32ReadCluster(Fat, Priv->DirCluster, DirBuf);

        Fat32DirEntry *Entry = (Fat32DirEntry*)(DirBuf + Priv->EntryOffset);
        Entry->FileSize = (UINT32)Inode->ISize;

        DateTime Dt;
        RtcReadTime(&Dt);

        Entry->WrtDate = FAT_DATE(Dt.Year, Dt.Month, Dt.Day);
        Entry->WrtTime = FAT_TIME(Dt.Hour, Dt.Minute, Dt.Second);

        Fat32WriteCluster(Fat, Priv->DirCluster, DirBuf);
        MemoryFree(DirBuf);
    }

    *Written = Done;
    return 0;
}

STATIC INT Fat32ReadDir(VfsInode *Dir, UINT64 *Pos, CHAR *Name,
                        UINT32 *NameLen, UINT32 *Type) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Dir->IPrivate;
    Fat32 *Fat = Priv->Fat;

    UINT32 EntryIndex = (UINT32)*Pos;
    UINT32 EntriesPerCluster = Fat->BytesPerCluster / 32;
    UINT32 ClusterIndex  = EntryIndex / EntriesPerCluster;
    UINT32 ClusterOffset = EntryIndex % EntriesPerCluster;

    UINT32 Cluster = Priv->FirstCluster;
    for (UINT32 I = 0; I < ClusterIndex; I++) {
        if (!Cluster || Cluster >= FAT32_EOC) return -1;
        Cluster = Fat32NextCluster(Fat, Cluster);
    }

    if (!Cluster || Cluster >= FAT32_EOC) return -1;

    UINT8 *Buf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
    if (!Buf) return -1;

    if (Fat32ReadCluster(Fat, Cluster, Buf) != 0) {
        MemoryFree(Buf);
        return -1;
    }

    Fat32DirEntry *Entry = (Fat32DirEntry*)(Buf + ClusterOffset * 32);

    while (Entry->Name[0] == 0xE5 || Entry->Attr == FAT_ATTR_LFN) {
        EntryIndex++;
        ClusterOffset++;

        if (ClusterOffset >= EntriesPerCluster) {
            Cluster = Fat32NextCluster(Fat, Cluster);
            if (!Cluster || Cluster >= FAT32_EOC) {
                MemoryFree(Buf);
                return -1;
            }
            if (Fat32ReadCluster(Fat, Cluster, Buf) != 0) {
                MemoryFree(Buf);
                return -1;
            }
            ClusterOffset = 0;
            Entry = (Fat32DirEntry*)Buf;
        } else {
            Entry++;
        }
    }

    if (Entry->Name[0] == 0) {
        MemoryFree(Buf);
        return -1;
    }

    Fat32NameFromEntry(Name, Entry->Name);
    *NameLen = (UINT32)StrLen(Name);
    *Type = (Entry->Attr & FAT_ATTR_DIRECTORY) ? FT_DIR : FT_REG_FILE;
    *Pos = EntryIndex + 1;

    MemoryFree(Buf);
    return 0;
}

STATIC INT Fat32Create(VfsInode *Dir, const CHAR *Name, UINT32 Mode, VfsInode **Result) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Dir->IPrivate;
    Fat32 *Fat = Priv->Fat;

    VfsInode *Existing;
    if (Fat32Lookup(Dir, Name, &Existing) == 0) {
        VfsInodeUnref(Existing);
        return -1;
    }

    UINT32 Cluster = Priv->FirstCluster;
    UINT8 *Buf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
    if (!Buf) return -1;

    UINT32 EntryIndex = 0;
    UINT32 TargetCluster = 0;
    UINT32 TargetOffset = 0;
    INT FoundFree = 0;

    while (Cluster && Cluster < FAT32_EOC) {
        if (Fat32ReadCluster(Fat, Cluster, Buf) != 0) {
            MemoryFree(Buf);
            return -1;
        }

        for (UINT32 I = 0; I < Fat->BytesPerCluster / 32; I++) {
            Fat32DirEntry *E = (Fat32DirEntry*)Buf + I;
            if (E->Name[0] == 0 || E->Name[0] == 0xE5) {
                TargetCluster = Cluster;
                TargetOffset = I * 32;
                FoundFree = 1;
                break;
            }
            EntryIndex++;
        }
        if (FoundFree) break;

        Cluster = Fat32NextCluster(Fat, Cluster);
    }

    if (!FoundFree) {
        UINT32 NewCluster = Fat32AllocCluster(Fat);
        if (!NewCluster) {
            MemoryFree(Buf);
            return -1;
        }

        if (!Priv->FirstCluster) {
            Priv->FirstCluster = NewCluster;
        } else {
            UINT32 Last = Priv->FirstCluster;
            UINT32 Next;
            while ((Next = Fat32NextCluster(Fat, Last)) && Next < FAT32_EOC)
                Last = Next;
            Fat->FatCache[Last] = NewCluster;
        }

        TargetCluster = NewCluster;
        TargetOffset = 0;

        MemSet(Buf, 0, Fat->BytesPerCluster);
        Fat32WriteCluster(Fat, NewCluster, Buf);
    } else if (TargetCluster != Cluster) {
        Fat32ReadCluster(Fat, TargetCluster, Buf);
    }

    UINT8 ShortName[11];
    UINT8 Checksum;
    Fat32NameToEntry(ShortName, Name, &Checksum);

    USIZE NameLen = StrLen(Name);
    INT LfnEntries = (NameLen > 12) ? (INT)((NameLen + 12) / 13) : 0;

    INT Offset = TargetOffset;

    if (LfnEntries > 0) {
        Fat32CreateLfnEntries(Fat, Buf, &Offset, Name, Checksum, 1);
    }

    Fat32DirEntry *Entry = (Fat32DirEntry*)(Buf + Offset);
    MemSet(Entry, 0, sizeof(Fat32DirEntry));
    MemCpy(Entry->Name, ShortName, 11);
    Entry->Attr = (Mode == FT_DIR) ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;

    DateTime Dt;
    RtcReadTime(&Dt);

    Entry->CrtDate = FAT_DATE(Dt.Year, Dt.Month, Dt.Day);
    Entry->CrtTime = FAT_TIME(Dt.Hour, Dt.Minute, Dt.Second);
    Entry->WrtDate = Entry->CrtDate;
    Entry->WrtTime = Entry->CrtTime;
    Entry->LstAccDate = Entry->CrtDate;

    if (Mode == FT_DIR) {
        UINT32 FirstCluster = Fat32AllocCluster(Fat);
        if (!FirstCluster) {
            MemoryFree(Buf);
            return -1;
        }

        Entry->FirstClusterLo = FirstCluster & 0xFFFF;
        Entry->FirstClusterHi = (FirstCluster >> 16) & 0xFFFF;

        UINT8 *DirBuf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
        if (!DirBuf) {
            MemoryFree(Buf);
            return -1;
        }
        MemSet(DirBuf, 0, Fat->BytesPerCluster);

        Fat32DirEntry *Dot = (Fat32DirEntry*)DirBuf;
        MemSet(Dot->Name, ' ', 11);
        Dot->Name[0] = '.';
        Dot->Attr = FAT_ATTR_DIRECTORY;
        Dot->FirstClusterLo = FirstCluster & 0xFFFF;
        Dot->FirstClusterHi = (FirstCluster >> 16) & 0xFFFF;
        Dot->CrtDate = Entry->CrtDate;
        Dot->CrtTime = Entry->CrtTime;

        Fat32DirEntry *DotDot = (Fat32DirEntry*)(DirBuf + 32);
        MemSet(DotDot->Name, ' ', 11);
        DotDot->Name[0] = '.';
        DotDot->Name[1] = '.';
        DotDot->Attr = FAT_ATTR_DIRECTORY;
        DotDot->FirstClusterLo = Priv->FirstCluster & 0xFFFF;
        DotDot->FirstClusterHi = (Priv->FirstCluster >> 16) & 0xFFFF;
        DotDot->CrtDate = Entry->CrtDate;
        DotDot->CrtTime = Entry->CrtTime;

        Fat32WriteCluster(Fat, FirstCluster, DirBuf);
        MemoryFree(DirBuf);
    }

    Fat32WriteCluster(Fat, TargetCluster, Buf);
    MemoryFree(Buf);

    VfsInode *Inode = VfsAllocInode();
    Fat32InodePrivate *NewPriv = (Fat32InodePrivate*)MemoryAllocate(sizeof(Fat32InodePrivate));

    UINT32 FirstCluster = (Entry->FirstClusterHi << 16) | Entry->FirstClusterLo;

    NewPriv->Fat = Fat;
    NewPriv->FirstCluster = FirstCluster;
    NewPriv->DirCluster = TargetCluster;
    NewPriv->DirEntry = EntryIndex;
    NewPriv->EntryOffset = Offset;
    NewPriv->ParentCluster = Priv->FirstCluster;

    Inode->IMode = Mode;
    Inode->ISize = 0;
    Inode->IPrivate = NewPriv;
    Inode->IOp = Dir->IOp;
    Inode->IFop = Dir->IFop;
    Inode->IDev = Fat->Dev;
    StrCpy(Inode->IFsName, "fat32");

    *Result = Inode;
    return 0;
}

STATIC INT Fat32Mkdir(VfsInode *Dir, const CHAR *Name, UINT32 Mode, VfsInode **Result) {
    (VOID)Mode;
    return Fat32Create(Dir, Name, FT_DIR, Result);
}

STATIC INT Fat32Unlink(VfsInode *Dir, const CHAR *Name) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Dir->IPrivate;
    Fat32 *Fat = Priv->Fat;

    VfsInode *Inode;
    if (Fat32Lookup(Dir, Name, &Inode) != 0) return -1;

    Fat32InodePrivate *FilePriv = (Fat32InodePrivate*)Inode->IPrivate;

    UINT32 Cluster = FilePriv->FirstCluster;
    UINT8 *ZeroBuf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);

    if (ZeroBuf) {
        MemSet(ZeroBuf, 0, Fat->BytesPerCluster);
        while (Cluster && Cluster < FAT32_EOC) {
            Fat32WriteCluster(Fat, Cluster, ZeroBuf);
            Cluster = Fat32NextCluster(Fat, Cluster);
        }
        MemoryFree(ZeroBuf);
    }

    if (FilePriv->FirstCluster)
        Fat32FreeClusterChain(Fat, FilePriv->FirstCluster);

    UINT8 *Buf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
    if (!Buf) {
        VfsInodeUnref(Inode);
        return -1;
    }

    Fat32ReadCluster(Fat, FilePriv->DirCluster, Buf);

    Fat32DirEntry *Entry = (Fat32DirEntry*)(Buf + FilePriv->EntryOffset);
    Entry->Name[0] = 0xE5;

    Fat32WriteCluster(Fat, FilePriv->DirCluster, Buf);
    MemoryFree(Buf);

    VfsInodeUnref(Inode);
    return 0;
}

STATIC INT Fat32Rmdir(VfsInode *Dir, const CHAR *Name) {
    return Fat32Unlink(Dir, Name);
}

STATIC INT Fat32Rename(VfsInode *OldDir, const CHAR *OldName,
                       VfsInode *NewDir, const CHAR *NewName) {
    Fat32InodePrivate *OldPriv = (Fat32InodePrivate*)OldDir->IPrivate;
    Fat32 *Fat = OldPriv->Fat;

    VfsInode *File;
    if (Fat32Lookup(OldDir, OldName, &File) != 0) return -1;

    Fat32InodePrivate *FilePriv = (Fat32InodePrivate*)File->IPrivate;

    UINT8 *OldBuf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
    if (!OldBuf) {
        VfsInodeUnref(File);
        return -1;
    }

    Fat32ReadCluster(Fat, FilePriv->DirCluster, OldBuf);

    UINT8 EntryData[32];
    MemCpy(EntryData, OldBuf + FilePriv->EntryOffset, 32);

    Fat32DirEntry *OldEntry = (Fat32DirEntry*)(OldBuf + FilePriv->EntryOffset);
    OldEntry->Name[0] = 0xE5;
    Fat32WriteCluster(Fat, FilePriv->DirCluster, OldBuf);

    Fat32InodePrivate *NewPriv = (Fat32InodePrivate*)NewDir->IPrivate;
    UINT32 NewCluster = NewPriv->FirstCluster;
    UINT8 *NewBuf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
    if (!NewBuf) {
        MemoryFree(OldBuf);
        VfsInodeUnref(File);
        return -1;
    }

    UINT32 NewOffset = 0;
    INT Found = 0;

    while (NewCluster && NewCluster < FAT32_EOC) {
        Fat32ReadCluster(Fat, NewCluster, NewBuf);

        for (UINT32 I = 0; I < Fat->BytesPerCluster / 32; I++) {
            Fat32DirEntry *E = (Fat32DirEntry*)NewBuf + I;
            if (E->Name[0] == 0 || E->Name[0] == 0xE5) {
                NewOffset = I * 32;
                Found = 1;
                break;
            }
        }
        if (Found) break;

        NewCluster = Fat32NextCluster(Fat, NewCluster);
    }

    if (!Found) {
        UINT32 Extra = Fat32AllocCluster(Fat);
        if (!Extra) {
            MemCpy(OldBuf + FilePriv->EntryOffset, EntryData, 32);
            Fat32WriteCluster(Fat, FilePriv->DirCluster, OldBuf);
            MemoryFree(OldBuf);
            MemoryFree(NewBuf);
            VfsInodeUnref(File);
            return -1;
        }

        if (!NewPriv->FirstCluster) {
            NewPriv->FirstCluster = Extra;
        } else {
            UINT32 Last = NewPriv->FirstCluster;
            UINT32 Next;
            while ((Next = Fat32NextCluster(Fat, Last)) && Next < FAT32_EOC)
                Last = Next;
            Fat->FatCache[Last] = Extra;
        }

        NewCluster = Extra;
        MemSet(NewBuf, 0, Fat->BytesPerCluster);
        NewOffset = 0;
    }

    UINT8 ShortName[11];
    Fat32NameToEntry(ShortName, NewName, NULLPTR);

    Fat32DirEntry *NewEntry = (Fat32DirEntry*)(EntryData);
    MemCpy(NewEntry->Name, ShortName, 11);

    MemCpy(NewBuf + NewOffset, EntryData, 32);
    Fat32WriteCluster(Fat, NewCluster, NewBuf);

    FilePriv->DirCluster = NewCluster;
    FilePriv->EntryOffset = NewOffset;
    FilePriv->ParentCluster = NewPriv->FirstCluster;

    MemoryFree(OldBuf);
    MemoryFree(NewBuf);
    VfsInodeUnref(File);
    return 0;
}

STATIC INT Fat32GetName(VfsInode *Inode, CHAR *Name, INT MaxLen) {
    (VOID)MaxLen;
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Inode->IPrivate;
    Fat32 *Fat = Priv->Fat;

    if (Priv->FirstCluster == Fat->RootCluster) {
        Name[0] = '\0';
        return 0;
    }

    UINT8 *Buf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
    if (!Buf) return -1;

    if (Fat32ReadCluster(Fat, Priv->DirCluster, Buf) != 0) {
        MemoryFree(Buf);
        return -1;
    }

    Fat32DirEntry *Entry = (Fat32DirEntry*)(Buf + Priv->EntryOffset);
    Fat32NameFromEntry(Name, Entry->Name);

    MemoryFree(Buf);
    return 0;
}

STATIC INT Fat32Chmod(VfsInode *Inode, UINT32 Mode) {
    Inode->IMode = Mode;
    return 0;
}

STATIC INT Fat32Stat(VfsInode *Inode, VOID *StatBuf) {
    return VfsStat(Inode, (VfsStats*)StatBuf);
}

STATIC INT Fat32Sync(VfsInode *Inode) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Inode->IPrivate;
    Fat32 *Fat = Priv->Fat;

    if (Fat->Dirty) {
        Fat32WriteSectors(Fat, Fat->FatStart, Fat->Bpb.FatSize32, Fat->FatCache);
        Fat32UpdateFsInfo(Fat);
    }

    return 0;
}

STATIC INT Fat32Parent(VfsInode *Inode, VfsInode **Parent) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Inode->IPrivate;
    Fat32 *Fat = Priv->Fat;

    if (Priv->FirstCluster == Fat->RootCluster) {
        *Parent = Inode;
        VfsInodeRef(*Parent);
        return 0;
    }

    VfsInode *ParentInode = VfsAllocInode();
    Fat32InodePrivate *ParentPriv =
        (Fat32InodePrivate*)MemoryAllocate(sizeof(Fat32InodePrivate));

    ParentPriv->Fat = Fat;
    ParentPriv->FirstCluster = Priv->ParentCluster;
    ParentPriv->DirCluster = 0;
    ParentPriv->DirEntry = 0;
    ParentPriv->EntryOffset = 0;
    ParentPriv->ParentCluster = Fat->RootCluster;

    ParentInode->IMode = FT_DIR;
    ParentInode->IPrivate = ParentPriv;
    ParentInode->IOp = Inode->IOp;
    ParentInode->IFop = Inode->IFop;
    ParentInode->IDev = Fat->Dev;
    StrCpy(ParentInode->IFsName, "fat32");

    *Parent = ParentInode;
    return 0;
}

STATIC INT Fat32Unmount(VfsInode *Root) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Root->IPrivate;
    Fat32 *Fat = Priv->Fat;

    if (Fat->Dirty) {
        Fat32WriteSectors(Fat, Fat->FatStart, Fat->Bpb.FatSize32, Fat->FatCache);
        Fat32UpdateFsInfo(Fat);
    }

    if (Fat->FatCache) MemoryFree(Fat->FatCache);
    MemoryFree(Fat);
    MemoryFree(Priv);

    return 0;
}

STATIC VfsOperations GFat32NodeOps = {
    .Lookup  = Fat32Lookup,
    .ReadDir = Fat32ReadDir,
    .Create  = Fat32Create,
    .Mkdir   = Fat32Mkdir,
    .Unlink  = Fat32Unlink,
    .Rmdir   = Fat32Rmdir,
    .Rename  = Fat32Rename,
    .Chmod   = Fat32Chmod,
    .Stat    = Fat32Stat,
    .GetName = Fat32GetName,
    .Parent  = Fat32Parent,
    .Unmount = Fat32Unmount,
};

STATIC VfsFileOperations GFat32FileOps = {
    .Read  = Fat32Read,
    .Write = Fat32Write,
    .Sync  = Fat32Sync,
};

// ============================================================================
// MOUNT
// ============================================================================

STATIC INT Fat32Mount(Disk *Dev, UINT64 StartLba, UINT64 SectorCount, VfsInode **Root) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "fat32: mounting...\n");

    Fat32 *Fat = (Fat32*)MemoryAllocate(sizeof(Fat32));
    if (!Fat) return -1;
    MemSet(Fat, 0, sizeof(Fat32));

    Fat->Dev = Dev;

    UINT8 Sector[512];
    if (Fat32ReadSectors(Fat, StartLba, 1, Sector) != 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "fat32: cannot read boot sector\n");
        MemoryFree(Fat);
        return -1;
    }

    Fat32Bpb *Bpb = (Fat32Bpb*)Sector;

    if (Bpb->BytesPerSector != 512 || Bpb->SectorsPerCluster == 0 ||
        Bpb->FatSize32 == 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "fat32: not a FAT32 volume\n");
        MemoryFree(Fat);
        return -1;
    }

    Fat->Bpb = *Bpb;
    Fat->BytesPerSector    = Bpb->BytesPerSector;
    Fat->SectorsPerCluster = Bpb->SectorsPerCluster;
    Fat->BytesPerCluster   = Fat->BytesPerSector * Fat->SectorsPerCluster;

    Fat->FatStart     = Bpb->ReservedSectors;
    Fat->DataStart    = Fat->FatStart + Bpb->NumFats * Bpb->FatSize32;
    Fat->RootCluster  = Bpb->RootCluster;
    Fat->FsinfoSector = Bpb->FsInfo;

    Fat->FatEntries    = Bpb->FatSize32 * Fat->BytesPerSector / 4;
    Fat->TotalClusters = (UINT32)((Bpb->TotalSectors32 - Fat->DataStart) /
                                  Fat->SectorsPerCluster);

    Fat->FatCache = (UINT32*)MemoryAllocate(Bpb->FatSize32 * 512);
    if (!Fat->FatCache) {
        MemoryFree(Fat);
        return -1;
    }

    if (Fat32ReadSectors(Fat, StartLba + Fat->FatStart, Bpb->FatSize32,
                         Fat->FatCache) != 0) {
        MemoryFree(Fat->FatCache);
        MemoryFree(Fat);
        return -1;
    }

    if (Fat->FsinfoSector) {
        Fat32ReadSectors(Fat, StartLba + Fat->FsinfoSector, 1, &Fat->FsInfo);
    } else {
        Fat->FsInfo.FreeCount = 0;
        Fat->FsInfo.NextFree = 2;
        for (UINT32 I = 2; I < Fat->FatEntries; I++) {
            if (Fat->FatCache[I] == FAT32_FREE)
                Fat->FsInfo.FreeCount++;
        }
    }

    VfsInode *RootInode = VfsAllocInode();
    Fat32InodePrivate *Priv =
        (Fat32InodePrivate*)MemoryAllocate(sizeof(Fat32InodePrivate));

    Priv->Fat = Fat;
    Priv->FirstCluster = Fat->RootCluster;
    Priv->DirCluster = 0;
    Priv->DirEntry = 0;
    Priv->EntryOffset = 0;
    Priv->ParentCluster = Fat->RootCluster;

    RootInode->IMode = FT_DIR;
    RootInode->ISize = 0;
    RootInode->IPrivate = Priv;
    RootInode->IOp = &GFat32NodeOps;
    RootInode->IFop = &GFat32FileOps;
    RootInode->IDev = Dev;
    StrCpy(RootInode->IFsName, "fat32");

    *Root = RootInode;

    BaseconPrintf(BASECON_TYPE_SUCCESS, "fat32: mounted successfully\n");
    return 0;
}

// ============================================================================
// FORMAT
// ============================================================================

INT Fat32Format(Disk *Dev, UINT64 StartLba, UINT64 SectorCount) {
    if (!Dev || !Dev->Write) {
        BaseconPrintf(BASECON_TYPE_ERROR, "fat32: device not ready\n");
        return -1;
    }

    UINT32 SectorSize = Dev->SectorSize;
    UINT64 TotalSectors = SectorCount ? SectorCount : Dev->SectorCount;

    UINT8 SectorsPerCluster;
    if (TotalSectors < 0x100000) SectorsPerCluster = 1;
    else if (TotalSectors < 0x400000) SectorsPerCluster = 4;
    else if (TotalSectors < 0x1000000) SectorsPerCluster = 8;
    else SectorsPerCluster = 16;

    UINT32 ReservedSectors = 32;
    UINT32 FatSize = (UINT32)((TotalSectors + SectorsPerCluster - 1) / SectorsPerCluster);
    FatSize = (FatSize * 4 + SectorSize - 1) / SectorSize;

    UINT32 DataStart = ReservedSectors + 2 * FatSize;
    UINT32 RootCluster = 2;

    Fat32Bpb Bpb;
    MemSet(&Bpb, 0, sizeof(Bpb));

    Bpb.JumpBoot[0] = 0xEB;
    Bpb.JumpBoot[1] = 0x58;
    Bpb.JumpBoot[2] = 0x90;
    MemCpy(Bpb.OemName, "TOS     ", 8);
    Bpb.BytesPerSector = 512;
    Bpb.SectorsPerCluster = SectorsPerCluster;
    Bpb.ReservedSectors = ReservedSectors;
    Bpb.NumFats = 2;
    Bpb.RootEntries = 0;
    Bpb.TotalSectors16 = 0;
    Bpb.MediaDescriptor = 0xF8;
    Bpb.FatSize16 = 0;
    Bpb.SectorsPerTrack = 63;
    Bpb.NumHeads = 255;
    Bpb.HiddenSectors = (UINT32)StartLba;
    Bpb.TotalSectors32 = (UINT32)TotalSectors;

    Bpb.FatSize32 = FatSize;
    Bpb.ExtendedFlags = 0;
    Bpb.FsVersion = 0;
    Bpb.RootCluster = RootCluster;
    Bpb.FsInfo = 1;
    Bpb.BackupBootSector = 6;
    Bpb.DriveNumber = 0x80;
    Bpb.Reserved1 = 0;
    Bpb.BootSignature = 0x29;
    Bpb.VolumeId = 0x12345678;
    MemCpy(Bpb.VolumeLabel, "TOSDISK    ", 11);
    MemCpy(Bpb.FsType, "FAT32   ", 8);

    UINT8 BootSector[512];
    MemSet(BootSector, 0, 512);
    MemCpy(BootSector, &Bpb, sizeof(Bpb));
    BootSector[510] = 0x55;
    BootSector[511] = 0xAA;

    if (Dev->Write(Dev, StartLba + 0, 1, BootSector) != 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "fat32: cannot write boot sector\n");
        return -1;
    }

    Dev->Write(Dev, StartLba + 6, 1, BootSector);

    Fat32FsInfo FsInfo;
    MemSet(&FsInfo, 0, sizeof(FsInfo));
    FsInfo.LeadSignature = 0x41615252;
    FsInfo.StructSignature = 0x61417272;
    FsInfo.FreeCount = (UINT32)((TotalSectors - DataStart) / SectorsPerCluster);
    FsInfo.NextFree = RootCluster + 1;
    FsInfo.TrailSignature = 0xAA550000;

    Dev->Write(Dev, StartLba + 1, 1, &FsInfo);
    Dev->Write(Dev, StartLba + 7, 1, &FsInfo);

    UINT32 *Fat = (UINT32*)MemoryAllocate(FatSize * SectorSize);
    if (!Fat) return -1;
    MemSet(Fat, 0, FatSize * SectorSize);

    Fat[0] = 0x0FFFFFF8;
    Fat[1] = 0x0FFFFFFF;
    Fat[RootCluster] = 0x0FFFFFFF;

    UINT32 BlockSize = 256;
    UINT8 *BlockBuf = (UINT8*)MemoryAllocate(BlockSize * SectorSize);
    if (!BlockBuf) {
        MemoryFree(Fat);
        return -1;
    }

    UINT32 LastPercent = 0xFFFFFFFF;
    BaseconPrintf(BASECON_TYPE_NORMAL, "[");

    for (INT FatNum = 0; FatNum < 2; FatNum++) {
        UINT32 FatStartSector = ReservedSectors + FatNum * FatSize;

        for (UINT32 I = 0; I < FatSize; I += BlockSize) {
            UINT32 Chunk = (I + BlockSize > FatSize) ? FatSize - I : BlockSize;

            for (UINT32 K = 0; K < Chunk; K++) {
                MemCpy(BlockBuf + K * SectorSize,
                       (UINT8*)Fat + (I + K) * SectorSize,
                       SectorSize);
            }

            if (Dev->Write(Dev, StartLba + FatStartSector + I, Chunk, BlockBuf) != 0) {
                MemoryFree(BlockBuf);
                MemoryFree(Fat);
                return -1;
            }

            UINT32 TotalWritten = (FatNum * FatSize + I + Chunk);
            UINT32 TotalToWrite = 2 * FatSize;
            UINT32 Percent = (TotalWritten * 100) / TotalToWrite;

            if (Percent != LastPercent) {
                UINT32 Pos = (Percent * 50) / 100;
                BaseconPrintf(BASECON_TYPE_NORMAL, "\r[");
                for (UINT32 J = 0; J < 50; J++) {
                    BaseconPrintf(BASECON_TYPE_NORMAL,
                                  "%s", J < Pos ? "=" : (J == Pos ? ">" : " "));
                }
                BaseconPrintf(BASECON_TYPE_NORMAL, "] %3u%%", Percent);
                LastPercent = Percent;
            }
        }
    }

    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    MemoryFree(BlockBuf);
    MemoryFree(Fat);

    UINT32 RootSectors = SectorsPerCluster;
    UINT8 *RootBuf = (UINT8*)MemoryAllocate(RootSectors * SectorSize);
    if (!RootBuf) return -1;
    MemSet(RootBuf, 0, RootSectors * SectorSize);

    Fat32DirEntry *Dot = (Fat32DirEntry*)RootBuf;
    MemSet(Dot->Name, ' ', 11);
    Dot->Name[0] = '.';
    Dot->Attr = FAT_ATTR_DIRECTORY;
    Dot->FirstClusterLo = RootCluster & 0xFFFF;
    Dot->FirstClusterHi = (RootCluster >> 16) & 0xFFFF;

    Fat32DirEntry *DotDot = (Fat32DirEntry*)(RootBuf + 32);
    MemSet(DotDot->Name, ' ', 11);
    DotDot->Name[0] = '.';
    DotDot->Name[1] = '.';
    DotDot->Attr = FAT_ATTR_DIRECTORY;
    DotDot->FirstClusterLo = RootCluster & 0xFFFF;
    DotDot->FirstClusterHi = (RootCluster >> 16) & 0xFFFF;

    Dev->Write(Dev, StartLba + DataStart, RootSectors, RootBuf);
    MemoryFree(RootBuf);

    if (Dev->Flush) Dev->Flush(Dev);

    INT CfgRc = DiskCfgWriteVolume(Dev, StartLba,
                                    SectorCount ? SectorCount : Dev->SectorCount,
                                    DISKCFG_FS_FAT32, "", FALSE);
    if (CfgRc != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_WARNING,
                      "fat32: disk.cfg not written (%d)\n", CfgRc);
    }

    return 0;
}

// ============================================================================
// INIT
// ============================================================================

STATIC FileSystem GFat32Fs = {
    .Name    = "fat32",
    .Mount   = Fat32Mount,
    .Unmount = Fat32Unmount,
    .Next    = NULLPTR,
};

VOID Fat32Init(VOID) {
    VfsRegisterFs(&GFat32Fs);
}