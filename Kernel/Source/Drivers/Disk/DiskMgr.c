#include <Drivers/Disk/DiskMgr.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Mem/Allocator.h>
#include <Return.h>
#include <Basecon.h>
#include <Drivers/Disk/Pata.h>

static ListHead GDiskList;
static UINT32 GDiskCount = 0;
static BOOL GInitialized = FALSE;

const CHAR* DiskTypeToString(DiskType Type) {
    switch (Type) {
        case DISK_TYPE_PATA: return "IDE";
        case DISK_TYPE_SATA: return "SATA";
        case DISK_TYPE_NVME: return "NVME";
        case DISK_TYPE_USB:  return "USB";
        default: return "UNKNOWN";
    }
}

const CHAR* PartitionTypeToString(PartitionType Type) {
    switch (Type) {
        case PARTITION_TYPE_FAT32:       return "FAT32";
        case PARTITION_TYPE_FAT16:       return "FAT16";
        case PARTITION_TYPE_NTFS:        return "NTFS";
        case PARTITION_TYPE_EXT2:        return "EXT2";
        case PARTITION_TYPE_EXT3:        return "EXT3";
        case PARTITION_TYPE_EXT4:        return "EXT4";
        case PARTITION_TYPE_LINUX_SWAP:  return "SWAP";
        case PARTITION_TYPE_EFI_SYSTEM:  return "EFI";
        case PARTITION_TYPE_FREE:        return "FREE";
        case PARTITION_TYPE_EXTENDED:    return "EXTENDED";
        default: return "UNKNOWN";
    }
}

const CHAR* PartitionIdToString(UINT8 Id) {
    switch (Id) {
        case 0x00: return "Empty";
        case 0x01: return "FAT12";
        case 0x04: return "FAT16 (DOS)";
        case 0x05: return "Extended";
        case 0x06: return "FAT16 (big)";
        case 0x07: return "NTFS/exFAT";
        case 0x0B: return "FAT32";
        case 0x0C: return "FAT32 (LBA)";
        case 0x0E: return "FAT16 (LBA)";
        case 0x0F: return "Extended (LBA)";
        case 0x82: return "Linux swap";
        case 0x83: return "Linux";
        case 0x85: return "Linux extended";
        case 0x8E: return "Linux LVM";
        case 0xEF: return "EFI System";
        default: return "Unknown";
    }
}

VOID DiskMakeName(Disk *Disk) {
    const CHAR *TypeStr = DiskTypeToString(Disk->Type);
    SnPrintf(Disk->Name, sizeof(Disk->Name), "%s%u", TypeStr, Disk->Index);
}

static VOID PartitionMakeName(Partition *Part) {
    if (!Part || !Part->ParentDisk) return;
    SnPrintf(Part->Name, sizeof(Part->Name), "%s:%u", 
             Part->ParentDisk->Name, Part->Index);
}

static VOID DiskClearPartitions(Disk *Disk) {
    if (!Disk) return;
    
    ListHead *Pos, *Next;
    
    ListForEachSafe(Pos, Next, &Disk->Partitions) {
        Partition *Part = ListEntry(Pos, Partition, Node);
        ListDel(&Part->Node);
        MemoryFree(Part);
    }
    Disk->PartitionCount = 0;
    ListInit(&Disk->Partitions);
    
    ListForEachSafe(Pos, Next, &Disk->LogicalPartitions) {
        Partition *Part = ListEntry(Pos, Partition, Node);
        ListDel(&Part->Node);
        MemoryFree(Part);
    }
    Disk->LogicalCount = 0;
    ListInit(&Disk->LogicalPartitions);
}

Disk* DiskMgrFindDisk(const CHAR *Name) {
    if (!GInitialized || !Name) return NULLPTR;
    
    ListHead *Pos;
    ListForEach(Pos, &GDiskList) {
        Disk *D = ListEntry(Pos, Disk, Node);
        if (StrCmp(D->Name, Name) == 0) {
            return D;
        }
    }
    return NULLPTR;
}

Partition* DiskMgrFindPartition(const CHAR *Name) {
    if (!GInitialized || !Name) return NULLPTR;
    
    CHAR DiskName[32];
    UINT32 PartIndex = 0;
    BOOL HasPart = FALSE;
    
    const CHAR *Colon = StrChr(Name, ':');
    if (Colon) {
        USIZE DiskLen = Colon - Name;
        if (DiskLen >= sizeof(DiskName)) DiskLen = sizeof(DiskName) - 1;
        StrnCpy(DiskName, Name, DiskLen);
        DiskName[DiskLen] = '\0';
        PartIndex = AToI(Colon + 1);
        HasPart = TRUE;
    } else {
        StrCpy(DiskName, Name);
    }
    
    Disk *Disk = DiskMgrFindDisk(DiskName);
    if (!Disk) return NULLPTR;
    
    if (!HasPart) return NULLPTR;
    
    ListHead *Pos;
    ListForEach(Pos, &Disk->Partitions) {
        Partition *Part = ListEntry(Pos, Partition, Node);
        if (Part->Index == PartIndex) {
            return Part;
        }
    }
    
    ListForEach(Pos, &Disk->LogicalPartitions) {
        Partition *Part = ListEntry(Pos, Partition, Node);
        if (Part->Index == PartIndex) {
            return Part;
        }
    }
    
    return NULLPTR;
}

UINT32 DiskMgrGetDiskCount(VOID) {
    return GDiskCount;
}

Disk* DiskMgrGetDisk(UINT32 Index) {
    ListHead *Pos;
    UINT32 Current = 0;
    
    ListForEach(Pos, &GDiskList) {
        if (Current == Index) {
            return ListEntry(Pos, Disk, Node);
        }
        Current++;
    }
    return NULLPTR;
}

Disk* DiskMgrGetFirst(VOID) {
    if (ListEmpty(&GDiskList)) return NULLPTR;
    return ListEntry(GDiskList.Next, Disk, Node);
}

Disk* DiskMgrGetNext(Disk *Prev) {
    if (!Prev) return NULLPTR;
    if (Prev->Node.Next == &GDiskList) return NULLPTR;
    return ListEntry(Prev->Node.Next, Disk, Node);
}

UINT32 DiskMgrGetPartitionCount(Disk *Disk) {
    if (!Disk) return 0;
    return Disk->PartitionCount;
}

Partition* DiskMgrGetPartition(Disk *Disk, UINT32 Index) {
    if (!Disk) return NULLPTR;
    
    ListHead *Pos;
    UINT32 Current = 0;
    
    ListForEach(Pos, &Disk->Partitions) {
        if (Current == Index) {
            return ListEntry(Pos, Partition, Node);
        }
        Current++;
    }
    return NULLPTR;
}

UINT32 DiskMgrGetLogicalCount(Disk *Disk) {
    if (!Disk) return 0;
    return Disk->LogicalCount;
}

Partition* DiskMgrGetLogical(Disk *Disk, UINT32 Index) {
    if (!Disk) return NULLPTR;
    
    ListHead *Pos;
    UINT32 Current = 0;
    
    ListForEach(Pos, &Disk->LogicalPartitions) {
        if (Current == Index) {
            return ListEntry(Pos, Partition, Node);
        }
        Current++;
    }
    return NULLPTR;
}

INT DiskMgrRead(const CHAR *Name, UINT64 Lba, UINT32 Count, VOID *Buffer) {
    Partition *Part = DiskMgrFindPartition(Name);
    if (Part) {
        return PartitionRead(Part, Lba, Count, Buffer);
    }
    
    Disk *Disk = DiskMgrFindDisk(Name);
    if (!Disk || !Disk->Read) RETURN(NO_OBJECT);
    
    return Disk->Read(Disk, Lba, Count, Buffer);
}

INT DiskMgrWrite(const CHAR *Name, UINT64 Lba, UINT32 Count, const VOID *Buffer) {
    Partition *Part = DiskMgrFindPartition(Name);
    if (Part) {
        return PartitionWrite(Part, Lba, Count, Buffer);
    }
    
    Disk *Disk = DiskMgrFindDisk(Name);
    if (!Disk || !Disk->Write) RETURN(NO_OBJECT);
    
    return Disk->Write(Disk, Lba, Count, Buffer);
}

INT PartitionRead(Partition *Part, UINT64 Offset, UINT32 Count, VOID *Buffer) {
    if (!Part || !Part->ParentDisk || !Part->ParentDisk->Read) RETURN(NO_OBJECT);
    if (Offset + Count > Part->SectorCount) RETURN(INCORRECT_VALUE);
    
    return Part->ParentDisk->Read(Part->ParentDisk, 
                                   Part->StartLba + Offset, 
                                   Count, Buffer);
}

INT PartitionWrite(Partition *Part, UINT64 Offset, UINT32 Count, const VOID *Buffer) {
    if (!Part || !Part->ParentDisk || !Part->ParentDisk->Write) RETURN(NO_OBJECT);
    if (Offset + Count > Part->SectorCount) RETURN(INCORRECT_VALUE);
    
    return Part->ParentDisk->Write(Part->ParentDisk,
                                    Part->StartLba + Offset,
                                    Count, Buffer);
}

INT PartitionFlush(Partition *Part) {
    if (!Part || !Part->ParentDisk || !Part->ParentDisk->Flush) RETURN(NO_OBJECT);
    return Part->ParentDisk->Flush(Part->ParentDisk);
}

UINT64 PartitionGetSize(Partition *Part) {
    if (!Part) return 0;
    return Part->SectorCount * Part->SectorSize;
}

typedef struct {
    UINT8 BootCode[446];
    struct {
        UINT8 BootFlag;
        UINT8 StartCHS[3];
        UINT8 PartitionId;
        UINT8 EndCHS[3];
        UINT32 StartLba;
        UINT32 SectorCount;
    } ATTRIBUTE(packed) Entries[4];
    UINT16 Signature;
} ATTRIBUTE(packed) MBRPartitionTable;

INT PartitionScanMBR(Disk *Disk) {
    if (!Disk) RETURN(NO_OBJECT);
    if (!Disk->Read) RETURN(NO_OBJECT);
    
    UINT8 MBR[512];
    INT Result = Disk->Read(Disk, 0, 1, MBR);
    if (Result != SUCCESS) RETURN(Result);
    
    MBRPartitionTable *Table = (MBRPartitionTable*)MBR;
    
    if (Table->Signature != 0xAA55) {
        RETURN(INCORRECT_VALUE);
    }
    
    // Clearing old partitions
    DiskClearPartitions(Disk);
    
    UINT32 PartIndex = 1;
    
    for (INT I = 0; I < 4; I++) {
        if (Table->Entries[I].SectorCount == 0) continue;
        if (Table->Entries[I].PartitionId == 0) continue;
        
        // Advanced section
        if (Table->Entries[I].PartitionId == 0x05 || 
            Table->Entries[I].PartitionId == 0x0F) {
            
            UINT64 ExtendedStart = Table->Entries[I].StartLba;
            UINT64 CurrentLba = ExtendedStart;
            
            while (1) {
                UINT8 Sector[512];
                Result = Disk->Read(Disk, CurrentLba, 1, Sector);
                if (Result != SUCCESS) break;
                
                MBRPartitionTable *ExtTable = (MBRPartitionTable*)Sector;
                if (ExtTable->Signature != 0xAA55) break;
                
                if (ExtTable->Entries[0].SectorCount > 0 && 
                    ExtTable->Entries[0].PartitionId != 0) {
                    
                    Partition *Part = (Partition*)MemoryAllocate(sizeof(Partition));
                    if (!Part) break;
                    
                    MemSet(Part, 0, sizeof(Partition));
                    Part->Index = PartIndex++;
                    Part->StartLba = ExtTable->Entries[0].StartLba;
                    Part->SectorCount = ExtTable->Entries[0].SectorCount;
                    Part->SectorSize = Disk->SectorSize;
                    Part->PartitionId = ExtTable->Entries[0].PartitionId;
                    Part->ParentDisk = Disk;
                    Part->SizeInBytes = Part->SectorCount * Part->SectorSize;
                    Part->Bootable = FALSE;
                    Part->ExtendedStartLba = CurrentLba;
                    
                    switch (Part->PartitionId) {
                        case 0x0B: case 0x0C: Part->Type = PARTITION_TYPE_FAT32; break;
                        case 0x06: case 0x0E: Part->Type = PARTITION_TYPE_FAT16; break;
                        case 0x07: Part->Type = PARTITION_TYPE_NTFS; break;
                        case 0x83: Part->Type = PARTITION_TYPE_EXT2; break;
                        case 0x82: Part->Type = PARTITION_TYPE_LINUX_SWAP; break;
                        default: Part->Type = PARTITION_TYPE_UNKNOWN; break;
                    }
                    
                    PartitionMakeName(Part);
                    ListAddTail(&Disk->LogicalPartitions, &Part->Node);
                    Disk->LogicalCount++;
                    
                    BaseconPrintf(BASECON_TYPE_NORMAL,
                                  "diskmgr: logical %s: start=0x%llX, size=%lluMB, type=%s\n",
                                  Part->Name,
                                  Part->StartLba,
                                  Part->SizeInBytes / (1024 * 1024),
                                  PartitionTypeToString(Part->Type));
                }
                
                if (ExtTable->Entries[1].SectorCount > 0) {
                    CurrentLba = ExtendedStart + ExtTable->Entries[1].StartLba;
                } else {
                    break;
                }
            }
            continue;
        }
        
        // Primary section
        Partition *Part = (Partition*)MemoryAllocate(sizeof(Partition));
        if (!Part) continue;
        
        MemSet(Part, 0, sizeof(Partition));
        Part->Index = PartIndex++;
        Part->StartLba = Table->Entries[I].StartLba;
        Part->SectorCount = Table->Entries[I].SectorCount;
        Part->SectorSize = Disk->SectorSize;
        Part->PartitionId = Table->Entries[I].PartitionId;
        Part->Bootable = (Table->Entries[I].BootFlag == 0x80);
        Part->ParentDisk = Disk;
        Part->SizeInBytes = Part->SectorCount * Part->SectorSize;
        
        switch (Part->PartitionId) {
            case 0x0B: case 0x0C: Part->Type = PARTITION_TYPE_FAT32; break;
            case 0x06: case 0x0E: Part->Type = PARTITION_TYPE_FAT16; break;
            case 0x07: Part->Type = PARTITION_TYPE_NTFS; break;
            case 0x83: Part->Type = PARTITION_TYPE_EXT2; break;
            case 0x82: Part->Type = PARTITION_TYPE_LINUX_SWAP; break;
            case 0xEF: Part->Type = PARTITION_TYPE_EFI_SYSTEM; break;
            default: Part->Type = PARTITION_TYPE_UNKNOWN; break;
        }
        
        PartitionMakeName(Part);
        ListAddTail(&Disk->Partitions, &Part->Node);
        Disk->PartitionCount++;
        
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "diskmgr: partition %s: start=0x%llX, size=%lluMB, type=%s%s\n",
                      Part->Name,
                      Part->StartLba,
                      Part->SizeInBytes / (1024 * 1024),
                      PartitionTypeToString(Part->Type),
                      Part->Bootable ? " BOOTABLE" : "");
    }
    
    RETURN(SUCCESS);
}

INT PartitionCreateMBR(Disk *Disk, UINT64 StartLba, UINT64 SectorCount, UINT8 PartitionId) {
    if (!Disk) RETURN(NO_OBJECT);
    if (Disk->PartitionCount >= 4) RETURN(NO_MEMORY);
    if (SectorCount == 0) RETURN(INCORRECT_VALUE);
    if (PartitionId == 0) RETURN(INCORRECT_VALUE);
    
    UINT8 MBR[512];
    INT Result = Disk->Read(Disk, 0, 1, MBR);
    if (Result != SUCCESS) RETURN(Result);
    
    MBRPartitionTable *Table = (MBRPartitionTable*)MBR;
    
    if (Table->Signature != 0xAA55) {
        RETURN(INCORRECT_VALUE);
    }
    
    INT FreeSlot = -1;
    for (INT I = 0; I < 4; I++) {
        if (Table->Entries[I].PartitionId == 0 || 
            Table->Entries[I].SectorCount == 0) {
            FreeSlot = I;
            break;
        }
    }
    
    if (FreeSlot == -1) RETURN(NO_MEMORY);
    
    Table->Entries[FreeSlot].BootFlag = 0x00;
    Table->Entries[FreeSlot].PartitionId = PartitionId;
    Table->Entries[FreeSlot].StartLba = (UINT32)StartLba;
    Table->Entries[FreeSlot].SectorCount = (UINT32)SectorCount;
    Table->Entries[FreeSlot].StartCHS[0] = 0xFF;
    Table->Entries[FreeSlot].StartCHS[1] = 0xFF;
    Table->Entries[FreeSlot].StartCHS[2] = 0xFF;
    Table->Entries[FreeSlot].EndCHS[0] = 0xFF;
    Table->Entries[FreeSlot].EndCHS[1] = 0xFF;
    Table->Entries[FreeSlot].EndCHS[2] = 0xFF;
    
    Result = Disk->Write(Disk, 0, 1, MBR);
    if (Result != SUCCESS) RETURN(Result);
    
    // Пересканируем
    PartitionScanMBR(Disk);
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "diskmgr: partition created on %s\n", Disk->Name);
    
    RETURN(SUCCESS);
}

INT PartitionDeleteMBR(Disk *Disk, UINT32 PartitionIndex) {
    if (!Disk) RETURN(NO_OBJECT);
    if (PartitionIndex == 0 || PartitionIndex > 4) RETURN(INCORRECT_VALUE);
    
    UINT8 MBR[512];
    INT Result = Disk->Read(Disk, 0, 1, MBR);
    if (Result != SUCCESS) RETURN(Result);
    
    MBRPartitionTable *Table = (MBRPartitionTable*)MBR;
    
    if (Table->Signature != 0xAA55) {
        RETURN(INCORRECT_VALUE);
    }
    
    INT Slot = PartitionIndex - 1;
    Table->Entries[Slot].BootFlag = 0;
    Table->Entries[Slot].PartitionId = 0;
    Table->Entries[Slot].StartLba = 0;
    Table->Entries[Slot].SectorCount = 0;
    
    Result = Disk->Write(Disk, 0, 1, MBR);
    if (Result != SUCCESS) RETURN(Result);
    
    PartitionScanMBR(Disk);
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "diskmgr: partition %u deleted from %s\n", 
                  PartitionIndex, Disk->Name);
    
    RETURN(SUCCESS);
}

VOID DiskMgrInit(VOID) {
    if (GInitialized) return;
    
    ListInit(&GDiskList);
    GDiskCount = 0;
    GInitialized = TRUE;
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "diskmgr: initialized\n");
}

INT DiskMgrRegisterDisk(Disk *Disk) {
    if (!Disk) RETURN(NO_OBJECT);
    if (!GInitialized) RETURN(NO_OBJECT);
    
    if (DiskMgrFindDisk(Disk->Name)) {
        RETURN(ALREADY_EXISTS);
    }
    
    ListInit(&Disk->Partitions);
    ListInit(&Disk->LogicalPartitions);
    Disk->PartitionCount = 0;
    Disk->LogicalCount = 0;
    
    ListAddTail(&GDiskList, &Disk->Node);
    GDiskCount++;
    
    if (Disk->Read) {
        PartitionScanMBR(Disk);
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "diskmgr: registered %s (%lluMB, %u partitions)\n",
                  Disk->Name,
                  Disk->TotalSize / (1024 * 1024),
                  Disk->PartitionCount + Disk->LogicalCount);
    
    RETURN(SUCCESS);
}

extern PataDrive PataDrives[4];

VOID DiskMgrInitDisks(VOID) {
    if (!GInitialized) {
        DiskMgrInit();
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "diskmgr: scanning disks...\n");

    INT PataFoundCount = 0;
    
    for (INT Ch = 0; Ch < 2; Ch++) {
        for (INT Dr = 0; Dr < 2; Dr++) {
            INT Idx = Ch * 2 + Dr;
            
            INT Result = PataInit(&PataDrives[Idx], (PataChannel)Ch, Dr);
            
            if (Result == SUCCESS && PataDrives[Idx].TotalSectors > 0) {
                PataFoundCount++;
            }
        }
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "diskmgr: %d PATA/IDE disks found\n", PataFoundCount);
}

