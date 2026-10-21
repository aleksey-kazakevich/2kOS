#include <Drivers/Disk/DiskMgr.h>
#include <Drivers/Disk/RamDisk.h>
#include <Drivers/Disk/Pata.h>
#include <Drivers/Disk/Sata.h>
#include <Fs/Fat32.h>
#include <Fs/Exfat.h>
#include <Fs/Iso9660.h>
#include <Fs/Vfs.h>
#include <Mem/Allocator.h>
#include <Mem/PageCache.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <SpinLock.h>
#include <Basecon.h>
#include <Asm/Cpu.h>
#include <Return.h>

/* ============================================================================
 * Global state
 * ============================================================================ */

ListHead GDiskList;
UINT32 GDiskCount = 0;
STATIC BOOL GInitialized = FALSE;
STATIC SysDevice *GDiskSysRoot = NULLPTR;
STATIC SpinLock GDiskLock;

extern VOID SataRegisterDisk(SataPort *Port, INT PortNum);
STATIC VOID DiskMgrRegisterOneDisk(Disk *Disk);

/* ============================================================================
 * PRNG for GPT GUID
 * ============================================================================ */

STATIC UINT64 GGptRngState = 0;
STATIC BOOL   GGptRngReady = FALSE;

STATIC BOOL CpuHasRdrand(VOID) {
    UINT32 Eax, Ebx, Ecx, Edx;
    Cpuid(1, &Eax, &Ebx, &Ecx, &Edx);
    return (Ecx & (1U << 30)) != 0;
}

STATIC BOOL Rdrand64(UINT64 *Out) {
    UINT8 Ok = 0;
    UINT64 Val = 0;
    for (INT I = 0; I < 10; I++) {
        asm volatile ("rdrand %0; setc %1"
                      : "=r"(Val), "=qm"(Ok));
        if (Ok) { *Out = Val; return TRUE; }
    }
    return FALSE;
}

STATIC UINT64 SplitMix64(UINT64 *State) {
    UINT64 Z = (*State += 0x9E3779B97F4A7C15ULL);
    Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBULL;
    return Z ^ (Z >> 31);
}

STATIC UINT64 XorShift64Star(UINT64 *State) {
    UINT64 X = *State;
    X ^= X >> 12;
    X ^= X << 25;
    X ^= X >> 27;
    *State = X;
    return X * 0x2545F4914F6CDD1DULL;
}

STATIC VOID GptRngSeed(VOID) {
    if (GGptRngReady) return;

    UINT64 Seed = 0;

    UINT64 Rd = 0;
    if (CpuHasRdrand() && Rdrand64(&Rd)) {
        Seed ^= Rd;
    }

    Seed ^= ReadTimeStampCounter();

    UINT64 Rsp;
    asm volatile ("mov %%rsp, %0" : "=r"(Rsp));
    Seed ^= Rsp;

    Seed ^= (UINT64)(UINTPTR)&GDiskList;
    Seed ^= (UINT64)(UINTPTR)&GGptRngState;

    GGptRngState = Seed;
    if (GGptRngState == 0) GGptRngState = 0x9E3779B97F4A7C15ULL;

    for (INT I = 0; I < 8; I++) {
        (VOID)SplitMix64(&GGptRngState);
    }

    GGptRngReady = TRUE;
}

STATIC UINT64 GptRand64Locked(VOID) {
    if (!GGptRngReady) GptRngSeed();
    return XorShift64Star(&GGptRngState);
}

STATIC UINT64 GptRand64(VOID) {
    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);
    UINT64 V = GptRand64Locked();
    SpinLockReleaseIrq(&GDiskLock, Flags);
    return V;
}

STATIC UINT32 GptRand32(VOID) {
    return (UINT32)GptRand64();
}

/* ============================================================================
 * Strings
 * ============================================================================ */

const CHAR* DiskTypeToString(DiskType Type) {
    switch (Type) {
        case DISK_TYPE_PATA:    return "IDE";
        case DISK_TYPE_SATA:    return "SATA";
        case DISK_TYPE_NVME:    return "NVME";
        case DISK_TYPE_USB:     return "USB";
        case DISK_TYPE_RAM:     return "RAM";
        case DISK_TYPE_OPTICAL: return "OPTICAL";
        default:                return "UNKNOWN";
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
        default:                         return "UNKNOWN";
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
        case 0xEE: return "GPT protective";
        case 0xEF: return "EFI System";
        default:   return "Unknown";
    }
}

const CHAR* DiskPartSchemeToString(DiskPartScheme Scheme) {
    switch (Scheme) {
        case DISK_PART_SCHEME_MBR: return "MBR";
        case DISK_PART_SCHEME_GPT: return "GPT";
        default:                   return "none";
    }
}

VOID DiskMakeName(Disk *Disk) {
    const CHAR *TypeStr = DiskTypeToString(Disk->Type);
    SnPrintf(Disk->Name, sizeof(Disk->Name), "%s%u", TypeStr, Disk->Index);
}

STATIC VOID PartitionMakeName(Partition *Part) {
    if (!Part || !Part->ParentDisk) return;
    if (Part->IsGpt) {
        SnPrintf(Part->Name, sizeof(Part->Name), "%sp%u",
                 Part->ParentDisk->Name, Part->Index);
    } else {
        SnPrintf(Part->Name, sizeof(Part->Name), "%s:%u",
                 Part->ParentDisk->Name, Part->Index);
    }
}

/* ============================================================================
 * Clearing partitions — under GDiskLock
 * ============================================================================ */

STATIC VOID DiskClearPartitionsLocked(Disk *Disk) {
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

/* ============================================================================
 * Search — under GDiskLock
 * ============================================================================ */

STATIC Disk* DiskMgrFindDiskLocked(const CHAR *Name) {
    if (!Name) return NULLPTR;

    ListHead *Pos;
    ListForEach(Pos, &GDiskList) {
        Disk *D = ListEntry(Pos, Disk, Node);
        if (StrCmp(D->Name, Name) == 0) return D;
        if (D->Alias[0] && StrCaseCmp(D->Alias, Name) == 0) return D;
    }
    return NULLPTR;
}

Disk* DiskMgrFindDisk(const CHAR *Name) {
    if (!GInitialized || !Name) return NULLPTR;

    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);
    Disk *D = DiskMgrFindDiskLocked(Name);
    SpinLockReleaseIrq(&GDiskLock, Flags);
    return D;
}

Partition* DiskMgrFindPartition(const CHAR *Name) {
    if (!GInitialized || !Name) return NULLPTR;

    CHAR DiskName[32];
    UINT32 PartIndex = 0;
    BOOL HasPart = FALSE;

    const CHAR *Colon = StrChr(Name, ':');
    if (Colon) {
        USIZE DiskLen = (USIZE)(Colon - Name);
        if (DiskLen >= sizeof(DiskName)) DiskLen = sizeof(DiskName) - 1;
        StrnCpy(DiskName, Name, DiskLen);
        DiskName[DiskLen] = '\0';
        PartIndex = (UINT32)AToI(Colon + 1);
        HasPart = TRUE;
    } else {
        const CHAR *P = StrrChr(Name, 'p');
        if (P && P != Name && (P[1] >= '0' && P[1] <= '9')) {
            USIZE DiskLen = (USIZE)(P - Name);
            if (DiskLen >= sizeof(DiskName)) DiskLen = sizeof(DiskName) - 1;
            StrnCpy(DiskName, Name, DiskLen);
            DiskName[DiskLen] = '\0';
            PartIndex = (UINT32)AToI(P + 1);
            HasPart = TRUE;
        } else {
            StrCpy(DiskName, Name);
        }
    }

    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);

    Disk *Disk = DiskMgrFindDiskLocked(DiskName);
    if (!Disk || !HasPart) {
        SpinLockReleaseIrq(&GDiskLock, Flags);
        return NULLPTR;
    }

    Partition *Result = NULLPTR;
    ListHead *Pos;
    ListForEach(Pos, &Disk->Partitions) {
        Partition *Part = ListEntry(Pos, Partition, Node);
        if (Part->Index == PartIndex) { Result = Part; break; }
    }
    if (!Result) {
        ListForEach(Pos, &Disk->LogicalPartitions) {
            Partition *Part = ListEntry(Pos, Partition, Node);
            if (Part->Index == PartIndex) { Result = Part; break; }
        }
    }

    SpinLockReleaseIrq(&GDiskLock, Flags);
    return Result;
}

UINT32 DiskMgrGetDiskCount(VOID) { return GDiskCount; }

Disk* DiskMgrGetDisk(UINT32 Index) {
    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);

    Disk *Result = NULLPTR;
    ListHead *Pos;
    UINT32 Current = 0;
    ListForEach(Pos, &GDiskList) {
        if (Current == Index) {
            Result = ListEntry(Pos, Disk, Node);
            break;
        }
        Current++;
    }

    SpinLockReleaseIrq(&GDiskLock, Flags);
    return Result;
}

Disk* DiskMgrGetFirst(VOID) {
    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);
    Disk *D = NULLPTR;
    if (!ListEmpty(&GDiskList)) {
        D = ListEntry(GDiskList.Next, Disk, Node);
    }
    SpinLockReleaseIrq(&GDiskLock, Flags);
    return D;
}

Disk* DiskMgrGetNext(Disk *Prev) {
    if (!Prev) return NULLPTR;

    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);
    Disk *D = NULLPTR;
    if (Prev->Node.Next != &GDiskList) {
        D = ListEntry(Prev->Node.Next, Disk, Node);
    }
    SpinLockReleaseIrq(&GDiskLock, Flags);
    return D;
}

UINT32 DiskMgrGetPartitionCount(Disk *Disk) {
    if (!Disk) return 0;
    return Disk->PartitionCount;
}

Partition* DiskMgrGetPartition(Disk *Disk, UINT32 Index) {
    if (!Disk) return NULLPTR;

    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);
    Partition *Result = NULLPTR;
    ListHead *Pos;
    UINT32 Current = 0;
    ListForEach(Pos, &Disk->Partitions) {
        if (Current == Index) {
            Result = ListEntry(Pos, Partition, Node);
            break;
        }
        Current++;
    }
    SpinLockReleaseIrq(&GDiskLock, Flags);
    return Result;
}

UINT32 DiskMgrGetLogicalCount(Disk *Disk) {
    if (!Disk) return 0;
    return Disk->LogicalCount;
}

Partition* DiskMgrGetLogical(Disk *Disk, UINT32 Index) {
    if (!Disk) return NULLPTR;

    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);
    Partition *Result = NULLPTR;
    ListHead *Pos;
    UINT32 Current = 0;
    ListForEach(Pos, &Disk->LogicalPartitions) {
        if (Current == Index) {
            Result = ListEntry(Pos, Partition, Node);
            break;
        }
        Current++;
    }
    SpinLockReleaseIrq(&GDiskLock, Flags);
    return Result;
}

/* ============================================================================
 * Read/write — they use a PCache that is already protected
 * ============================================================================ */

INT DiskMgrRead(const CHAR *Name, UINT64 Lba, UINT32 Count, VOID *Buffer) {
    Partition *Part = DiskMgrFindPartition(Name);
    if (Part) return PartitionRead(Part, Lba, Count, Buffer);

    Disk *Disk = DiskMgrFindDisk(Name);
    if (!Disk || !Disk->Read) RETURN(NO_OBJECT);
    return PCacheRead(Disk, Lba, Count, Buffer);
}

INT DiskMgrWrite(const CHAR *Name, UINT64 Lba, UINT32 Count, const VOID *Buffer) {
    Partition *Part = DiskMgrFindPartition(Name);
    if (Part) return PartitionWrite(Part, Lba, Count, Buffer);

    Disk *Disk = DiskMgrFindDisk(Name);
    if (!Disk || !Disk->Write) RETURN(NO_OBJECT);
    return PCacheWrite(Disk, Lba, Count, Buffer);
}

INT PartitionRead(Partition *Part, UINT64 Offset, UINT32 Count, VOID *Buffer) {
    if (!Part || !Part->ParentDisk || !Part->ParentDisk->Read) RETURN(NO_OBJECT);
    if (Offset + Count > Part->SectorCount) RETURN(INCORRECT_VALUE);
    return PCacheRead(Part->ParentDisk, Part->StartLba + Offset, Count, Buffer);
}

INT PartitionWrite(Partition *Part, UINT64 Offset, UINT32 Count, const VOID *Buffer) {
    if (!Part || !Part->ParentDisk || !Part->ParentDisk->Write) RETURN(NO_OBJECT);
    if (Offset + Count > Part->SectorCount) RETURN(INCORRECT_VALUE);
    return PCacheWrite(Part->ParentDisk, Part->StartLba + Offset, Count, Buffer);
}

INT PartitionFlush(Partition *Part) {
    if (!Part || !Part->ParentDisk || !Part->ParentDisk->Flush) RETURN(NO_OBJECT);
    return PCacheFlush(Part->ParentDisk);
}

UINT64 PartitionGetSize(Partition *Part) {
    if (!Part) return 0;
    return Part->SectorCount * Part->SectorSize;
}

/* ============================================================================
 * MBR
 * ============================================================================ */

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

STATIC INT PartitionScanMBRLocked(Disk *Disk) {
    if (!Disk) RETURN(NO_OBJECT);
    if (!Disk->Read) RETURN(NO_OBJECT);
    if (Disk->SectorSize != 512) RETURN(NOT_SUPPORTED);

    UINT8 MBR[512];
    INT Result = Disk->Read(Disk, 0, 1, MBR);
    if (Result != SUCCESS) RETURN(Result);

    MBRPartitionTable *Table = (MBRPartitionTable*)MBR;
    if (Table->Signature != 0xAA55) RETURN(INCORRECT_VALUE);

    DiskClearPartitionsLocked(Disk);
    Disk->PartScheme = DISK_PART_SCHEME_MBR;

    UINT32 PartIndex = 1;

    for (INT I = 0; I < 4; I++) {
        if (Table->Entries[I].SectorCount == 0) continue;
        if (Table->Entries[I].PartitionId == 0) continue;

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
                    Part->IsGpt = FALSE;

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
                                  Part->Name, Part->StartLba,
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
        Part->IsGpt = FALSE;

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
                      Part->Name, Part->StartLba,
                      Part->SizeInBytes / (1024 * 1024),
                      PartitionTypeToString(Part->Type),
                      Part->Bootable ? " BOOTABLE" : "");
    }

    RETURN(SUCCESS);
}

INT PartitionScanMBR(Disk *Disk) {
    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);
    INT Rc = PartitionScanMBRLocked(Disk);
    SpinLockReleaseIrq(&GDiskLock, Flags);
    return Rc;
}

/* ============================================================================
 * GPT
 * ============================================================================ */

#define GPT_SIGNATURE           0x5452415020494645ULL
#define GPT_REVISION_1_0        0x00010000
#define GPT_ENTRY_MIN_SIZE      128
#define GPT_MAX_ENTRIES         128
#define GPT_PROTECTIVE_MBR_ID   0xEE
#define GPT_ENTRY_NAME_BYTES    72

typedef struct ATTRIBUTE(packed) {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} GUID;

typedef struct ATTRIBUTE(packed) {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 HeaderCrc32;
    UINT32 Reserved;
    UINT64 MyLba;
    UINT64 AlternateLba;
    UINT64 FirstUsableLba;
    UINT64 LastUsableLba;
    GUID   DiskGuid;
    UINT64 PartitionEntryLba;
    UINT32 NumberOfPartitionEntries;
    UINT32 SizeOfPartitionEntry;
    UINT32 PartitionEntryArrayCrc32;
} GptHeader;

typedef struct ATTRIBUTE(packed) {
    GUID   TypeGuid;
    GUID   UniqueGuid;
    UINT64 FirstLba;
    UINT64 LastLba;
    UINT64 Attributes;
    CHAR   Name[GPT_ENTRY_NAME_BYTES];
} GptEntry;

_Static_assert(sizeof(GptEntry) == 128, "GPT entry must be 128 bytes");

STATIC const GUID GGptTypeEfiSystem = {
    0xC12A7328, 0xF81F, 0x11D2,
    {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}
};

STATIC const GUID GGptTypeMsBasicData = {
    0xEBD0A0A2, 0xB9E5, 0x4433,
    {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}
};

STATIC const GUID GGptTypeMsReserved = {
    0xE3C9E316, 0x0B5C, 0x4DB8,
    {0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE}
};

STATIC const GUID GGptTypeLinuxFilesystem = {
    0x0FC63DAF, 0x8483, 0x4772,
    {0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4}
};

STATIC const GUID GGptTypeLinuxSwap = {
    0x0657FD6D, 0xA4AB, 0x43C4,
    {0x84, 0xE5, 0x09, 0x33, 0xC3, 0x4B, 0x4F, 0x4E}
};

STATIC BOOL GuidIsZero(const GUID *G) {
    if (G->Data1 || G->Data2 || G->Data3) return FALSE;
    for (INT I = 0; I < 8; I++) if (G->Data4[I]) return FALSE;
    return TRUE;
}

STATIC BOOL GuidEq(const GUID *A, const GUID *B) {
    if (A->Data1 != B->Data1 || A->Data2 != B->Data2 ||
        A->Data3 != B->Data3) return FALSE;
    for (INT I = 0; I < 8; I++) {
        if (A->Data4[I] != B->Data4[I]) return FALSE;
    }
    return TRUE;
}

STATIC VOID GuidCopy(GUID *Dst, const GUID *Src) {
    MemCpy(Dst, Src, sizeof(GUID));
}

STATIC VOID GptGenerateGuidLocked(GUID *Out) {
    if (!Out) return;

    UINT64 Hi = GptRand64Locked();
    UINT64 Lo = GptRand64Locked();

    Out->Data1 = (UINT32)(Hi >> 32);
    Out->Data2 = (UINT16)(Hi >> 16);
    Out->Data3 = (UINT16)(Hi & 0xFFFF);
    Out->Data3 = (UINT16)((Out->Data3 & 0x0FFF) | 0x4000);

    for (INT I = 0; I < 8; I++) {
        Out->Data4[I] = (UINT8)(Lo >> (I * 8));
    }

    Out->Data4[0] = (UINT8)((Out->Data4[0] & 0x3F) | 0x80);
}

STATIC VOID GptNameToAscii(const CHAR *Utf16, INT Utf16Bytes,
                            CHAR *Out, INT OutMax) {
    INT OutPos = 0;
    for (INT I = 0; I + 1 < Utf16Bytes && OutPos < OutMax - 1; I += 2) {
        UINT16 C = (UINT8)Utf16[I] | ((UINT16)(UINT8)Utf16[I+1] << 8);
        if (C == 0) break;
        Out[OutPos++] = (C < 0x80) ? (CHAR)C : '?';
    }
    Out[OutPos] = '\0';
}

STATIC VOID GptNameFromAscii(const CHAR *Ascii, CHAR *Out, INT OutBytes) {
    MemSet(Out, 0, (USIZE)OutBytes);
    if (!Ascii) return;
    INT I = 0;
    for (; I < OutBytes - 2 && Ascii[I]; I++) {
        Out[I * 2 + 0] = Ascii[I];
        Out[I * 2 + 1] = 0;
    }
}

STATIC PartitionType GptTypeToPartitionType(const GUID *G) {
    if (GuidIsZero(G)) return PARTITION_TYPE_FREE;
    if (GuidEq(G, &GGptTypeEfiSystem))       return PARTITION_TYPE_EFI_SYSTEM;
    if (GuidEq(G, &GGptTypeLinuxSwap))       return PARTITION_TYPE_LINUX_SWAP;
    if (GuidEq(G, &GGptTypeLinuxFilesystem)) return PARTITION_TYPE_EXT4;
    if (GuidEq(G, &GGptTypeMsBasicData))     return PARTITION_TYPE_NTFS;
    if (GuidEq(G, &GGptTypeMsReserved))      return PARTITION_TYPE_UNKNOWN;
    return PARTITION_TYPE_UNKNOWN;
}

STATIC const GUID *PartitionTypeToGptGuid(PartitionType Type) {
    switch (Type) {
        case PARTITION_TYPE_EFI_SYSTEM:  return &GGptTypeEfiSystem;
        case PARTITION_TYPE_LINUX_SWAP:  return &GGptTypeLinuxSwap;
        case PARTITION_TYPE_EXT2:
        case PARTITION_TYPE_EXT3:
        case PARTITION_TYPE_EXT4:        return &GGptTypeLinuxFilesystem;
        case PARTITION_TYPE_NTFS:
        case PARTITION_TYPE_FAT32:
        case PARTITION_TYPE_FAT16:       return &GGptTypeMsBasicData;
        default:                         return &GGptTypeMsBasicData;
    }
}

STATIC UINT8 PartitionTypeToSynthId(PartitionType Type) {
    switch (Type) {
        case PARTITION_TYPE_EFI_SYSTEM:  return 0xEF;
        case PARTITION_TYPE_NTFS:        return 0x07;
        case PARTITION_TYPE_FAT32:       return 0x0C;
        case PARTITION_TYPE_FAT16:       return 0x0E;
        case PARTITION_TYPE_LINUX_SWAP:  return 0x82;
        case PARTITION_TYPE_EXT4:        return 0x83;
        default:                         return 0xEE;
    }
}

STATIC UINT32 Crc32(const VOID *Data, USIZE Len) {
    STATIC UINT32 Table[256];
    STATIC BOOL TableReady = FALSE;

    if (!TableReady) {
        for (UINT32 I = 0; I < 256; I++) {
            UINT32 C = I;
            for (INT K = 0; K < 8; K++) {
                C = (C & 1) ? (0xEDB88320U ^ (C >> 1)) : (C >> 1);
            }
            Table[I] = C;
        }
        TableReady = TRUE;
    }

    UINT32 Crc = 0xFFFFFFFFU;
    const UINT8 *P = (const UINT8 *)Data;
    for (USIZE I = 0; I < Len; I++) {
        Crc = Table[(Crc ^ P[I]) & 0xFF] ^ (Crc >> 8);
    }
    return Crc ^ 0xFFFFFFFFU;
}

STATIC INT GptReadHeader(Disk *Disk, GptHeader *Out) {
    UINT32 SecSize = Disk->SectorSize;
    UINT8 *Buf = (UINT8 *)MemoryAllocate(SecSize);
    if (!Buf) RETURN(NO_MEMORY);

    INT Rc = Disk->Read(Disk, 1, 1, Buf);
    if (Rc != SUCCESS) { MemoryFree(Buf); RETURN(Rc); }

    MemCpy(Out, Buf, sizeof(GptHeader));
    MemoryFree(Buf);

    if (Out->Signature != GPT_SIGNATURE) RETURN(NOT_FOUND);
    if (Out->HeaderSize < sizeof(GptHeader) || Out->HeaderSize > 512) {
        RETURN(INCORRECT_VALUE);
    }
    if (Out->NumberOfPartitionEntries == 0 ||
        Out->SizeOfPartitionEntry < GPT_ENTRY_MIN_SIZE ||
        Out->SizeOfPartitionEntry > 4096) {
        RETURN(INCORRECT_VALUE);
    }

    UINT32 SavedCrc = Out->HeaderCrc32;
    GptHeader Tmp;
    MemCpy(&Tmp, Out, sizeof(GptHeader));
    Tmp.HeaderCrc32 = 0;
    UINT32 CalcCrc = Crc32(&Tmp, Out->HeaderSize);
    if (CalcCrc != SavedCrc) {
        BaseconPrintf(BASECON_TYPE_WARNING,
                      "diskmgr: GPT header CRC mismatch (0x%08X vs 0x%08X)\n",
                      CalcCrc, SavedCrc);
    }

    RETURN(SUCCESS);
}

STATIC INT GptReadEntries(Disk *Disk, const GptHeader *Hdr,
                          UINT8 **OutBuf, USIZE *OutLen) {
    USIZE Len = (USIZE)Hdr->NumberOfPartitionEntries * Hdr->SizeOfPartitionEntry;
    UINT8 *Buf = (UINT8 *)MemoryAllocate(Len);
    if (!Buf) RETURN(NO_MEMORY);

    UINT32 Sectors = (UINT32)((Len + Disk->SectorSize - 1) / Disk->SectorSize);
    INT Rc = Disk->Read(Disk, Hdr->PartitionEntryLba, Sectors, Buf);
    if (Rc != SUCCESS) {
        MemoryFree(Buf);
        RETURN(Rc);
    }

    UINT32 CalcCrc = Crc32(Buf, Len);
    if (CalcCrc != Hdr->PartitionEntryArrayCrc32) {
        BaseconPrintf(BASECON_TYPE_WARNING,
                      "diskmgr: GPT entries CRC mismatch (0x%08X vs 0x%08X)\n",
                      CalcCrc, Hdr->PartitionEntryArrayCrc32);
    }

    *OutBuf = Buf;
    *OutLen = Len;
    RETURN(SUCCESS);
}

STATIC INT PartitionScanGPTLocked(Disk *Disk) {
    if (!Disk || !Disk->Read) RETURN(NO_OBJECT);
    if (Disk->SectorSize < 512) RETURN(NOT_SUPPORTED);

    GptHeader Hdr;
    INT Rc = GptReadHeader(Disk, &Hdr);
    if (Rc != SUCCESS) RETURN(Rc);

    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "diskmgr: GPT: %u entries, entry_size=%u, table_lba=%llu\n",
                  Hdr.NumberOfPartitionEntries,
                  Hdr.SizeOfPartitionEntry,
                  Hdr.PartitionEntryLba);

    UINT8 *Table = NULLPTR;
    USIZE TableLen = 0;
    Rc = GptReadEntries(Disk, &Hdr, &Table, &TableLen);
    if (Rc != SUCCESS) RETURN(Rc);

    DiskClearPartitionsLocked(Disk);
    Disk->PartScheme = DISK_PART_SCHEME_GPT;
    MemCpy(Disk->DiskGuid, &Hdr.DiskGuid, 16);

    UINT32 PartIndex = 1;
    UINT32 EntrySize = Hdr.SizeOfPartitionEntry;

    for (UINT32 I = 0; I < Hdr.NumberOfPartitionEntries; I++) {
        GptEntry *E = (GptEntry *)(Table + (USIZE)I * EntrySize);

        if (GuidIsZero(&E->TypeGuid)) continue;
        if (E->LastLba < E->FirstLba) continue;

        Partition *Part = (Partition *)MemoryAllocate(sizeof(Partition));
        if (!Part) continue;
        MemSet(Part, 0, sizeof(Partition));

        Part->Index       = PartIndex++;
        Part->StartLba    = E->FirstLba;
        Part->SectorCount = E->LastLba - E->FirstLba + 1;
        Part->SectorSize  = Disk->SectorSize;
        Part->ParentDisk  = Disk;
        Part->SizeInBytes = Part->SectorCount * Part->SectorSize;
        Part->Bootable    = FALSE;
        Part->IsGpt       = TRUE;
        Part->Attributes  = E->Attributes;
        Part->Type        = GptTypeToPartitionType(&E->TypeGuid);
        Part->PartitionId = PartitionTypeToSynthId(Part->Type);
        MemCpy(Part->TypeGuid,   &E->TypeGuid,   16);
        MemCpy(Part->UniqueGuid, &E->UniqueGuid, 16);

        GptNameToAscii(E->Name, sizeof(E->Name), Part->Label, sizeof(Part->Label));

        PartitionMakeName(Part);
        ListAddTail(&Disk->Partitions, &Part->Node);
        Disk->PartitionCount++;

        if (Part->Label[0]) {
            BaseconPrintf(BASECON_TYPE_NORMAL,
                          "diskmgr: gpt %s: start=%llu, size=%lluMB, type=%s, label='%s'\n",
                          Part->Name, Part->StartLba,
                          Part->SizeInBytes / (1024 * 1024),
                          PartitionTypeToString(Part->Type),
                          Part->Label);
        } else {
            BaseconPrintf(BASECON_TYPE_NORMAL,
                          "diskmgr: gpt %s: start=%llu, size=%lluMB, type=%s\n",
                          Part->Name, Part->StartLba,
                          Part->SizeInBytes / (1024 * 1024),
                          PartitionTypeToString(Part->Type));
        }
    }

    MemoryFree(Table);

    BaseconPrintf(BASECON_TYPE_SUCCESS,
                  "diskmgr: GPT parsed, %u partition(s)\n", Disk->PartitionCount);
    RETURN(SUCCESS);
}

INT PartitionScanGPT(Disk *Disk) {
    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);
    INT Rc = PartitionScanGPTLocked(Disk);
    SpinLockReleaseIrq(&GDiskLock, Flags);
    return Rc;
}

STATIC INT PartitionScanLocked(Disk *Disk) {
    if (!Disk || !Disk->Read) RETURN(NO_OBJECT);
    if (Disk->SectorSize < 512) RETURN(NOT_SUPPORTED);

    UINT8 MBR[512];
    INT Rc = Disk->Read(Disk, 0, 1, MBR);
    if (Rc != SUCCESS) RETURN(Rc);

    MBRPartitionTable *Table = (MBRPartitionTable *)MBR;

    if (Table->Signature != 0xAA55) {
        BaseconPrintf(BASECON_TYPE_INFO,
                      "diskmgr: %s: no MBR/GPT signature at LBA 0\n",
                      Disk->Name);
        DiskClearPartitionsLocked(Disk);
        Disk->PartScheme = DISK_PART_SCHEME_NONE;
        RETURN(INCORRECT_VALUE);
    }

    BOOL HasProtective = FALSE;
    for (INT I = 0; I < 4; I++) {
        if (Table->Entries[I].PartitionId == GPT_PROTECTIVE_MBR_ID &&
            Table->Entries[I].SectorCount > 0) {
            HasProtective = TRUE;
            break;
        }
    }

    if (HasProtective) {
        BaseconPrintf(BASECON_TYPE_INFO,
                      "diskmgr: %s: protective MBR, trying GPT...\n",
                      Disk->Name);

        Rc = PartitionScanGPTLocked(Disk);
        if (Rc == SUCCESS) return SUCCESS;

        BaseconPrintf(BASECON_TYPE_WARNING,
                      "diskmgr: %s: GPT parse failed (%d), MBR fallback\n",
                      Disk->Name, Rc);
    }

    return PartitionScanMBRLocked(Disk);
}

INT PartitionScan(Disk *Disk) {
    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);
    INT Rc = PartitionScanLocked(Disk);
    SpinLockReleaseIrq(&GDiskLock, Flags);
    return Rc;
}

STATIC INT PartitionCreateGPTLocked(Disk *Disk, UINT64 StartLba,
                                     UINT64 SectorCount, PartitionType Type,
                                     const CHAR *Label) {
    if (!Disk || !Disk->Read || !Disk->Write) RETURN(NO_OBJECT);
    if (SectorCount == 0) RETURN(INCORRECT_VALUE);
    if (StartLba + SectorCount > Disk->SectorCount) RETURN(INCORRECT_VALUE);

    GptHeader Hdr;
    INT Rc = GptReadHeader(Disk, &Hdr);
    if (Rc != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "diskmgr: %s: cannot read GPT header (%d)\n",
                      Disk->Name, Rc);
        RETURN(Rc);
    }

    UINT8 *Table = NULLPTR;
    USIZE TableLen = 0;
    Rc = GptReadEntries(Disk, &Hdr, &Table, &TableLen);
    if (Rc != SUCCESS) RETURN(Rc);

    UINT32 EntrySize = Hdr.SizeOfPartitionEntry;
    INT FreeIdx = -1;

    for (UINT32 I = 0; I < Hdr.NumberOfPartitionEntries; I++) {
        GptEntry *E = (GptEntry *)(Table + (USIZE)I * EntrySize);
        if (!GuidIsZero(&E->TypeGuid) && E->FirstLba == StartLba) {
            FreeIdx = (INT)I;
            break;
        }
    }
    if (FreeIdx < 0) {
        for (UINT32 I = 0; I < Hdr.NumberOfPartitionEntries; I++) {
            GptEntry *E = (GptEntry *)(Table + (USIZE)I * EntrySize);
            if (GuidIsZero(&E->TypeGuid)) {
                FreeIdx = (INT)I;
                break;
            }
        }
    }

    if (FreeIdx < 0) {
        MemoryFree(Table);
        BaseconPrintf(BASECON_TYPE_ERROR, "diskmgr: no free GPT slots\n");
        RETURN(NO_MEMORY);
    }

    GptEntry *E = (GptEntry *)(Table + (USIZE)FreeIdx * EntrySize);
    const GUID *TypeGuid = PartitionTypeToGptGuid(Type);

    GuidCopy(&E->TypeGuid, TypeGuid);
    GptGenerateGuidLocked(&E->UniqueGuid);

    E->FirstLba   = StartLba;
    E->LastLba    = StartLba + SectorCount - 1;
    E->Attributes = 0;
    GptNameFromAscii(Label ? Label : "", E->Name, sizeof(E->Name));

    UINT32 TableCrc = Crc32(Table, TableLen);
    UINT32 Sectors = (UINT32)((TableLen + Disk->SectorSize - 1) / Disk->SectorSize);

    Rc = Disk->Write(Disk, Hdr.PartitionEntryLba, Sectors, Table);
    if (Rc != SUCCESS) {
        MemoryFree(Table);
        RETURN(Rc);
    }

    Hdr.PartitionEntryArrayCrc32 = TableCrc;
    Hdr.HeaderCrc32 = 0;
    Hdr.HeaderCrc32 = Crc32(&Hdr, Hdr.HeaderSize);

    UINT8 *HdrSec = (UINT8 *)MemoryAllocate(Disk->SectorSize);
    if (!HdrSec) { MemoryFree(Table); RETURN(NO_MEMORY); }
    MemSet(HdrSec, 0, Disk->SectorSize);
    MemCpy(HdrSec, &Hdr, sizeof(GptHeader));
    Rc = Disk->Write(Disk, 1, 1, HdrSec);
    MemoryFree(HdrSec);
    if (Rc != SUCCESS) { MemoryFree(Table); RETURN(Rc); }

    /* Backup GPT */
    UINT64 BackupHdrLba = Disk->SectorCount - 1;
    UINT64 BackupEntriesLba = BackupHdrLba - Sectors;

    Hdr.MyLba = BackupHdrLba;
    Hdr.AlternateLba = 1;
    Hdr.PartitionEntryLba = BackupEntriesLba;
    Hdr.HeaderCrc32 = 0;
    Hdr.HeaderCrc32 = Crc32(&Hdr, Hdr.HeaderSize);

    Rc = Disk->Write(Disk, BackupEntriesLba, Sectors, Table);
    if (Rc == SUCCESS) {
        UINT8 *BkHdr = (UINT8 *)MemoryAllocate(Disk->SectorSize);
        if (BkHdr) {
            MemSet(BkHdr, 0, Disk->SectorSize);
            MemCpy(BkHdr, &Hdr, sizeof(GptHeader));
            Disk->Write(Disk, BackupHdrLba, 1, BkHdr);
            MemoryFree(BkHdr);
        }
    }

    MemoryFree(Table);

    if (Disk->Flush) Disk->Flush(Disk);

    BaseconPrintf(BASECON_TYPE_SUCCESS,
                  "diskmgr: GPT partition created at LBA %llu (%llu sectors)\n",
                  StartLba, SectorCount);

    PartitionScanGPTLocked(Disk);
    RETURN(SUCCESS);
}

INT PartitionCreateGPT(Disk *Disk, UINT64 StartLba, UINT64 SectorCount,
                       PartitionType Type, const CHAR *Label) {
    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);
    INT Rc = PartitionCreateGPTLocked(Disk, StartLba, SectorCount, Type, Label);
    SpinLockReleaseIrq(&GDiskLock, Flags);
    return Rc;
}

STATIC INT PartitionDeleteGPTLocked(Disk *Disk, UINT32 PartitionIndex) {
    if (!Disk || !Disk->Read || !Disk->Write) RETURN(NO_OBJECT);

    GptHeader Hdr;
    INT Rc = GptReadHeader(Disk, &Hdr);
    if (Rc != SUCCESS) RETURN(Rc);

    UINT8 *Table = NULLPTR;
    USIZE TableLen = 0;
    Rc = GptReadEntries(Disk, &Hdr, &Table, &TableLen);
    if (Rc != SUCCESS) RETURN(Rc);

    UINT32 EntrySize = Hdr.SizeOfPartitionEntry;
    INT Found = -1;
    UINT32 Seen = 0;

    for (UINT32 I = 0; I < Hdr.NumberOfPartitionEntries; I++) {
        GptEntry *E = (GptEntry *)(Table + (USIZE)I * EntrySize);
        if (GuidIsZero(&E->TypeGuid)) continue;
        Seen++;
        if (Seen == PartitionIndex) { Found = (INT)I; break; }
    }

    if (Found < 0) { MemoryFree(Table); RETURN(NOT_FOUND); }

    GptEntry *E = (GptEntry *)(Table + (USIZE)Found * EntrySize);
    MemSet(E, 0, EntrySize);

    UINT32 TableCrc = Crc32(Table, TableLen);
    UINT32 Sectors = (UINT32)((TableLen + Disk->SectorSize - 1) / Disk->SectorSize);

    Rc = Disk->Write(Disk, Hdr.PartitionEntryLba, Sectors, Table);
    if (Rc != SUCCESS) { MemoryFree(Table); RETURN(Rc); }

    Hdr.PartitionEntryArrayCrc32 = TableCrc;
    Hdr.HeaderCrc32 = 0;
    Hdr.HeaderCrc32 = Crc32(&Hdr, Hdr.HeaderSize);

    UINT8 *HdrSec = (UINT8 *)MemoryAllocate(Disk->SectorSize);
    if (!HdrSec) { MemoryFree(Table); RETURN(NO_MEMORY); }
    MemSet(HdrSec, 0, Disk->SectorSize);
    MemCpy(HdrSec, &Hdr, sizeof(GptHeader));
    Rc = Disk->Write(Disk, 1, 1, HdrSec);
    MemoryFree(HdrSec);

    MemoryFree(Table);
    if (Disk->Flush) Disk->Flush(Disk);

    PartitionScanGPTLocked(Disk);
    RETURN(SUCCESS);
}

INT PartitionDeleteGPT(Disk *Disk, UINT32 PartitionIndex) {
    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);
    INT Rc = PartitionDeleteGPTLocked(Disk, PartitionIndex);
    SpinLockReleaseIrq(&GDiskLock, Flags);
    return Rc;
}

STATIC INT GptInitDiskLocked(Disk *Disk) {
    if (!Disk || !Disk->Read || !Disk->Write) RETURN(NO_OBJECT);
    if (Disk->SectorSize < 512) RETURN(NOT_SUPPORTED);
    if (Disk->SectorCount < 128) RETURN(INCORRECT_VALUE);

    UINT32 SecSize = Disk->SectorSize;

    UINT8 *MBR = (UINT8 *)MemoryAllocate(SecSize);
    if (!MBR) RETURN(NO_MEMORY);
    MemSet(MBR, 0, SecSize);

    MBRPartitionTable *Table = (MBRPartitionTable *)MBR;
    Table->Entries[0].BootFlag = 0x00;
    Table->Entries[0].PartitionId = GPT_PROTECTIVE_MBR_ID;
    Table->Entries[0].StartLba = 1;
    UINT32 ProtectiveSectors = (Disk->SectorCount > 0xFFFFFFFFULL)
                             ? 0xFFFFFFFFU : (UINT32)Disk->SectorCount;
    Table->Entries[0].SectorCount = ProtectiveSectors;
    Table->Entries[0].StartCHS[0] = 0x00;
    Table->Entries[0].StartCHS[1] = 0x02;
    Table->Entries[0].StartCHS[2] = 0x00;
    Table->Entries[0].EndCHS[0] = 0xFF;
    Table->Entries[0].EndCHS[1] = 0xFF;
    Table->Entries[0].EndCHS[2] = 0xFF;
    Table->Signature = 0xAA55;

    INT Rc = Disk->Write(Disk, 0, 1, MBR);
    MemoryFree(MBR);
    if (Rc != SUCCESS) RETURN(Rc);

    UINT32 EntryCount = GPT_MAX_ENTRIES;
    UINT32 EntrySize = sizeof(GptEntry);
    USIZE TableLen = (USIZE)EntryCount * EntrySize;
    UINT32 TableSectors = (UINT32)((TableLen + SecSize - 1) / SecSize);

    UINT8 *Entries = (UINT8 *)MemoryAllocate(TableLen);
    if (!Entries) RETURN(NO_MEMORY);
    MemSet(Entries, 0, TableLen);

    UINT64 EntriesLba = 2;
    UINT64 FirstUsable = EntriesLba + TableSectors;
    UINT64 LastUsable  = Disk->SectorCount - 1 - TableSectors - 1;

    if (FirstUsable >= LastUsable) {
        MemoryFree(Entries);
        RETURN(INCORRECT_VALUE);
    }

    Rc = Disk->Write(Disk, EntriesLba, TableSectors, Entries);
    if (Rc != SUCCESS) { MemoryFree(Entries); RETURN(Rc); }

    UINT32 EntriesCrc = Crc32(Entries, TableLen);

    GptHeader Hdr;
    MemSet(&Hdr, 0, sizeof(Hdr));
    Hdr.Signature = GPT_SIGNATURE;
    Hdr.Revision = GPT_REVISION_1_0;
    Hdr.HeaderSize = sizeof(GptHeader);
    Hdr.MyLba = 1;
    Hdr.AlternateLba = Disk->SectorCount - 1;
    Hdr.FirstUsableLba = FirstUsable;
    Hdr.LastUsableLba = LastUsable;

    GptGenerateGuidLocked(&Hdr.DiskGuid);

    Hdr.PartitionEntryLba = EntriesLba;
    Hdr.NumberOfPartitionEntries = EntryCount;
    Hdr.SizeOfPartitionEntry = EntrySize;
    Hdr.PartitionEntryArrayCrc32 = EntriesCrc;
    Hdr.HeaderCrc32 = 0;
    Hdr.HeaderCrc32 = Crc32(&Hdr, Hdr.HeaderSize);

    UINT8 *HdrSec = (UINT8 *)MemoryAllocate(SecSize);
    if (!HdrSec) { MemoryFree(Entries); RETURN(NO_MEMORY); }
    MemSet(HdrSec, 0, SecSize);
    MemCpy(HdrSec, &Hdr, sizeof(GptHeader));
    Rc = Disk->Write(Disk, 1, 1, HdrSec);
    MemoryFree(HdrSec);
    if (Rc != SUCCESS) { MemoryFree(Entries); RETURN(Rc); }

    UINT64 BackupHdrLba = Disk->SectorCount - 1;
    UINT64 BackupEntriesLba = BackupHdrLba - TableSectors;

    Rc = Disk->Write(Disk, BackupEntriesLba, TableSectors, Entries);
    if (Rc != SUCCESS) { MemoryFree(Entries); RETURN(Rc); }

    Hdr.MyLba = BackupHdrLba;
    Hdr.AlternateLba = 1;
    Hdr.PartitionEntryLba = BackupEntriesLba;
    Hdr.HeaderCrc32 = 0;
    Hdr.HeaderCrc32 = Crc32(&Hdr, Hdr.HeaderSize);

    HdrSec = (UINT8 *)MemoryAllocate(SecSize);
    if (!HdrSec) { MemoryFree(Entries); RETURN(NO_MEMORY); }
    MemSet(HdrSec, 0, SecSize);
    MemCpy(HdrSec, &Hdr, sizeof(GptHeader));
    Rc = Disk->Write(Disk, BackupHdrLba, 1, HdrSec);
    MemoryFree(HdrSec);

    MemoryFree(Entries);

    if (Disk->Flush) Disk->Flush(Disk);

    BaseconPrintf(BASECON_TYPE_SUCCESS,
                  "diskmgr: GPT initialized on %s (usable %llu..%llu)\n",
                  Disk->Name, FirstUsable, LastUsable);

    PartitionScanGPTLocked(Disk);
    RETURN(SUCCESS);
}

INT GptInitDisk(Disk *Disk) {
    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);
    INT Rc = GptInitDiskLocked(Disk);
    SpinLockReleaseIrq(&GDiskLock, Flags);
    return Rc;
}

/* ============================================================================
 * MBR: create/delete
 * ============================================================================ */

STATIC INT PartitionCreateMBRLocked(Disk *Disk, UINT64 StartLba,
                                     UINT64 SectorCount, UINT8 PartitionId) {
    if (!Disk) RETURN(NO_OBJECT);
    if (Disk->PartitionCount >= 4) RETURN(NO_MEMORY);
    if (SectorCount == 0 || PartitionId == 0) RETURN(INCORRECT_VALUE);
    if (StartLba + SectorCount > Disk->SectorCount) RETURN(INCORRECT_VALUE);
    if (Disk->PartScheme == DISK_PART_SCHEME_GPT) RETURN(NOT_SUPPORTED);

    UINT8 MBR[512];
    INT Result = Disk->Read(Disk, 0, 1, MBR);
    if (Result != SUCCESS) RETURN(Result);

    MBRPartitionTable *Table = (MBRPartitionTable*)MBR;

    if (Table->Signature != 0xAA55) {
        MemSet(MBR, 0, 512);
        MBR[510] = 0x55;
        MBR[511] = 0xAA;
        Table = (MBRPartitionTable*)MBR;
        Result = Disk->Write(Disk, 0, 1, MBR);
        if (Result != SUCCESS) RETURN(Result);
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

    PartitionScanMBRLocked(Disk);
    BaseconPrintf(BASECON_TYPE_SUCCESS,
                  "diskmgr: MBR partition created on %s\n", Disk->Name);
    RETURN(SUCCESS);
}

INT PartitionCreateMBR(Disk *Disk, UINT64 StartLba,
                       UINT64 SectorCount, UINT8 PartitionId) {
    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);
    INT Rc = PartitionCreateMBRLocked(Disk, StartLba, SectorCount, PartitionId);
    SpinLockReleaseIrq(&GDiskLock, Flags);
    return Rc;
}

STATIC INT PartitionDeleteMBRLocked(Disk *Disk, UINT32 PartitionIndex) {
    if (!Disk) RETURN(NO_OBJECT);
    if (PartitionIndex == 0 || PartitionIndex > 4) RETURN(INCORRECT_VALUE);
    if (Disk->PartScheme == DISK_PART_SCHEME_GPT) RETURN(NOT_SUPPORTED);

    UINT8 MBR[512];
    INT Result = Disk->Read(Disk, 0, 1, MBR);
    if (Result != SUCCESS) RETURN(Result);

    MBRPartitionTable *Table = (MBRPartitionTable*)MBR;
    if (Table->Signature != 0xAA55) RETURN(INCORRECT_VALUE);

    INT Slot = (INT)(PartitionIndex - 1);
    Table->Entries[Slot].BootFlag = 0;
    Table->Entries[Slot].PartitionId = 0;
    Table->Entries[Slot].StartLba = 0;
    Table->Entries[Slot].SectorCount = 0;

    Result = Disk->Write(Disk, 0, 1, MBR);
    if (Result != SUCCESS) RETURN(Result);

    PartitionScanMBRLocked(Disk);
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "diskmgr: MBR partition %u deleted from %s\n",
                  PartitionIndex, Disk->Name);
    RETURN(SUCCESS);
}

INT PartitionDeleteMBR(Disk *Disk, UINT32 PartitionIndex) {
    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);
    INT Rc = PartitionDeleteMBRLocked(Disk, PartitionIndex);
    SpinLockReleaseIrq(&GDiskLock, Flags);
    return Rc;
}

/* ============================================================================
 * DiskCfg: FS + apply detector
 * ============================================================================ */

STATIC DiskCfgFsKind DiskCfg_DetectFs(Disk *D, UINT64 StartLba) {
    if (!D || !D->Read) return DISKCFG_FS_NONE;

    UINT8 Sector[512];
    if (D->Read(D, StartLba, 1, Sector) != SUCCESS)
        return DISKCFG_FS_NONE;

    if (MemCmp(Sector + 3, "EXFAT   ", 8) == 0)
        return DISKCFG_FS_EXFAT;

    Fat32Bpb *B = (Fat32Bpb*)Sector;
    if (B->BytesPerSector == 512 && B->FatSize32 != 0 &&
        B->RootCluster >= 2)
        return DISKCFG_FS_FAT32;

    if (D->SectorSize == 2048) {
        UINT8 Pvd[ISO_SECTOR_SIZE];
        if (D->Read(D, 16, 1, Pvd) == SUCCESS &&
            MemCmp(Pvd + 1, "CD001", 5) == 0 &&
            Pvd[0] == 0x01)
            return DISKCFG_FS_ISO9660;
    }

    return DISKCFG_FS_NONE;
}

INT DiskCfgApplyToDisk(Disk *D) {
    if (!D) RETURN(NO_OBJECT);
    if (!VfsRootInode) RETURN(NO_OBJECT);
    if (D->Type == DISK_TYPE_RAM) RETURN(NOT_SUPPORTED);
    if (D->Type == DISK_TYPE_OPTICAL) RETURN(NOT_SUPPORTED);
    if (!D->Read) RETURN(NO_OBJECT);

    if (D->PartitionCount > 0 || D->LogicalCount > 0) RETURN(SUCCESS);

    DiskCfgFsKind Fs = DiskCfg_DetectFs(D, 0);
    D->CfgFsKind = Fs;
    if (Fs == DISKCFG_FS_NONE || Fs == DISKCFG_FS_ISO9660)
        RETURN(NOT_FOUND);

    DiskCfg Cfg;
    INT Rc = DiskCfgReadVolume(D, 0, D->SectorCount, Fs, &Cfg);
    if (Rc != SUCCESS) RETURN(Rc);

    StrnCpy(D->CfgId, Cfg.Id, sizeof(D->CfgId) - 1);

    if (Cfg.Stale) {
        D->CfgStale = TRUE;
        BaseconPrintf(BASECON_TYPE_WARNING,
                      "diskcg: %s: id mismatch, config ignored\n",
                      D->Name);
        RETURN(INCORRECT_VALUE);
    }

    D->CfgValid = TRUE;

    if (Cfg.Name[0]) {
        if (!DiskCfgAliasExists(Cfg.Name, D, NULLPTR)) {
            StrnCpy(D->Alias, Cfg.Name, sizeof(D->Alias) - 1);
            BaseconPrintf(BASECON_TYPE_NORMAL,
                          "diskcg: %s: alias '%s'\n",
                          D->Name, D->Alias);
        } else {
            BaseconPrintf(BASECON_TYPE_WARNING,
                          "diskcg: %s: alias '%s' already used, ignored\n",
                          D->Name, Cfg.Name);
        }
    }

    D->IsRoot = Cfg.IsRoot;
    if (D->IsRoot) {
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "diskcg: %s: is_root=1\n", D->Name);
    }

    RETURN(SUCCESS);
}

INT DiskCfgApplyToPartition(Partition *P) {
    if (!P || !P->ParentDisk) RETURN(NO_OBJECT);
    if (!VfsRootInode) RETURN(NO_OBJECT);
    if (!P->ParentDisk->Read) RETURN(NO_OBJECT);

    if (P->Type == PARTITION_TYPE_EFI_SYSTEM ||
        P->Type == PARTITION_TYPE_LINUX_SWAP ||
        P->Type == PARTITION_TYPE_UNKNOWN ||
        P->Type == PARTITION_TYPE_EXTENDED)
        RETURN(NOT_SUPPORTED);

    Disk *D = P->ParentDisk;

    DiskCfgFsKind Fs = DiskCfg_DetectFs(D, P->StartLba);
    P->CfgFsKind = Fs;
    if (Fs == DISKCFG_FS_NONE || Fs == DISKCFG_FS_ISO9660)
        RETURN(NOT_FOUND);

    DiskCfg Cfg;
    INT Rc = DiskCfgReadVolume(D, P->StartLba, P->SectorCount, Fs, &Cfg);
    if (Rc != SUCCESS) RETURN(Rc);

    StrnCpy(P->CfgId, Cfg.Id, sizeof(P->CfgId) - 1);

    if (Cfg.Stale) {
        P->CfgStale = TRUE;
        BaseconPrintf(BASECON_TYPE_WARNING,
                      "diskcg: %s: id mismatch, config ignored\n",
                      P->Name);
        RETURN(INCORRECT_VALUE);
    }

    P->CfgValid = TRUE;

    if (Cfg.Name[0]) {
        if (!DiskCfgAliasExists(Cfg.Name, NULLPTR, P)) {
            StrnCpy(P->Alias, Cfg.Name, sizeof(P->Alias) - 1);
            BaseconPrintf(BASECON_TYPE_NORMAL,
                          "diskcg: %s: alias '%s'\n",
                          P->Name, P->Alias);
        } else {
            BaseconPrintf(BASECON_TYPE_WARNING,
                          "diskcg: %s: alias '%s' already used, ignored\n",
                          P->Name, Cfg.Name);
        }
    }

    P->IsRoot = Cfg.IsRoot;
    if (P->IsRoot) {
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "diskcg: %s: is_root=1\n", P->Name);
    }

    RETURN(SUCCESS);
}

BOOL DiskCfgAliasExists(const CHAR *Alias, Disk *SelfD, Partition *SelfP) {
    if (!Alias || !Alias[0]) return FALSE;

    Disk *D = DiskMgrGetFirst();
    while (D) {
        if (D != SelfD && StrCaseCmp(D->Alias, Alias) == 0) return TRUE;
        for (UINT32 I = 0; I < D->PartitionCount; I++) {
            Partition *P = DiskMgrGetPartition(D, I);
            if (P && P != SelfP && StrCaseCmp(P->Alias, Alias) == 0)
                return TRUE;
        }
        for (UINT32 I = 0; I < D->LogicalCount; I++) {
            Partition *P = DiskMgrGetLogical(D, I);
            if (P && P != SelfP && StrCaseCmp(P->Alias, Alias) == 0)
                return TRUE;
        }
        D = DiskMgrGetNext(D);
    }
    return FALSE;
}

VOID DiskCfgMountRoots(VOID) {
    if (!VfsRootInode) {
        BaseconPrintf(BASECON_TYPE_WARNING,
                      "diskcg: VFS not ready, cannot mount roots\n");
        return;
    }

    INT Mounted = 0;
    INT Errors = 0;
    CHAR FirstMountedName[DRIVE_NAME_MAX] = {0};

    Disk *D = DiskMgrGetFirst();
    while (D) {
        if (D->IsRoot && D->PartitionCount == 0 && D->LogicalCount == 0) {
            if (D->Type != DISK_TYPE_RAM &&
                D->Type != DISK_TYPE_OPTICAL &&
                D->CfgFsKind != DISKCFG_FS_NONE) {

                const CHAR *FsName = NULLPTR;
                if (D->CfgFsKind == DISKCFG_FS_FAT32) FsName = "fat32";
                else if (D->CfgFsKind == DISKCFG_FS_EXFAT) FsName = "exfat";

                if (FsName) {
                    VfsInode *Root = NULLPTR;
                    INT Rc = VfsMount(FsName, D, 0, D->SectorCount, &Root);
                    if (Rc == SUCCESS) {
                        BaseconPrintf(BASECON_TYPE_SUCCESS,
                                      "diskcg: mounted %s (is_root)\n",
                                      D->Name);
                        VfsInodeUnref(Root);
			if (Mounted == 0) {
                            StrnCpy(FirstMountedName, D->Name,
                                    DRIVE_NAME_MAX - 1);
                        }
                        Mounted++;
                    } else {
                        BaseconPrintf(BASECON_TYPE_ERROR,
                                      "diskcg: %s: mount failed (%d)\n",
                                      D->Name, Rc);
                        Errors++;
                    }
                }
            }
        }

        for (UINT32 I = 0; I < D->PartitionCount; I++) {
            Partition *P = DiskMgrGetPartition(D, I);
            if (!P || !P->IsRoot) continue;

            const CHAR *FsName = NULLPTR;
            if (P->CfgFsKind == DISKCFG_FS_FAT32) FsName = "fat32";
            else if (P->CfgFsKind == DISKCFG_FS_EXFAT) FsName = "exfat";
            if (!FsName) continue;

            VfsInode *Root = NULLPTR;
            INT Rc = VfsMount(FsName, D, P->StartLba, P->SectorCount, &Root);
            if (Rc == SUCCESS) {
                BaseconPrintf(BASECON_TYPE_SUCCESS,
                              "diskcg: mounted %s (is_root)\n", P->Name);
                VfsInodeUnref(Root);
		if (Mounted == 0) {
                    StrnCpy(FirstMountedName, P->Name,
                            DRIVE_NAME_MAX - 1);
                }
                Mounted++;
            } else {
                BaseconPrintf(BASECON_TYPE_ERROR,
                              "diskcg: %s: mount failed (%d)\n",
                              P->Name, Rc);
                Errors++;
            }
        }

        for (UINT32 I = 0; I < D->LogicalCount; I++) {
            Partition *P = DiskMgrGetLogical(D, I);
            if (!P || !P->IsRoot) continue;

            const CHAR *FsName = NULLPTR;
            if (P->CfgFsKind == DISKCFG_FS_FAT32) FsName = "fat32";
            else if (P->CfgFsKind == DISKCFG_FS_EXFAT) FsName = "exfat";
            if (!FsName) continue;

            VfsInode *Root = NULLPTR;
            INT Rc = VfsMount(FsName, D, P->StartLba, P->SectorCount, &Root);
            if (Rc == SUCCESS) {
                BaseconPrintf(BASECON_TYPE_SUCCESS,
                              "diskcg: mounted %s (is_root)\n", P->Name);
                VfsInodeUnref(Root);
		if (Mounted == 0) {
                    StrnCpy(FirstMountedName, P->Name,
                            DRIVE_NAME_MAX - 1);
                }

                Mounted++;
            } else {
                BaseconPrintf(BASECON_TYPE_ERROR,
                              "diskcg: %s: mount failed (%d)\n",
                              P->Name, Rc);
                Errors++;
            }
        }

        D = DiskMgrGetNext(D);
    }

    if (Mounted == 0 && Errors == 0) {
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "diskcg: no is_root volumes\n");
    } else {
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "diskcg: mounted %d root volume(s)\n", Mounted);
	if (FirstMountedName[0]) {
            VfsCd(&VfsCurrentDir, FirstMountedName);
        }
    }
}

/* ============================================================================
 * DiskCfg: main entry point (after InitFs)
 * ============================================================================ */

VOID DiskCfgInit(VOID) {
    if (!VfsRootInode) {
        BaseconPrintf(BASECON_TYPE_WARNING,
                      "diskcg: VFS not ready, cannot read cfgs\n");
        return;
    }

    BaseconPrintf(BASECON_TYPE_NORMAL, "diskcg: reading disk.cfg...\n");

    Disk *D = DiskMgrGetFirst();
    while (D) {
        DiskCfgApplyToDisk(D);

        for (UINT32 I = 0; I < D->PartitionCount; I++) {
            Partition *P = DiskMgrGetPartition(D, I);
            if (P) DiskCfgApplyToPartition(P);
        }
        for (UINT32 I = 0; I < D->LogicalCount; I++) {
            Partition *P = DiskMgrGetLogical(D, I);
            if (P) DiskCfgApplyToPartition(P);
        }
        D = DiskMgrGetNext(D);
    }

    DiskCfgMountRoots();
}

/* ============================================================================
 * Disk Registration
 * ============================================================================ */

VOID DiskMgrInit(VOID) {
    if (GInitialized) return;
    SpinLockInit(&GDiskLock);
    ListInit(&GDiskList);
    GDiskCount = 0;
    GInitialized = TRUE;

    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);
    GptRngSeed();
    SpinLockReleaseIrq(&GDiskLock, Flags);

    BaseconPrintf(BASECON_TYPE_NORMAL, "diskmgr: initialized\n");
}

INT DiskMgrRegisterDisk(Disk *Disk) {
    if (!Disk) RETURN(NO_OBJECT);
    if (!GInitialized) RETURN(NO_OBJECT);

    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);

    if (DiskMgrFindDiskLocked(Disk->Name)) {
        SpinLockReleaseIrq(&GDiskLock, Flags);
        RETURN(ALREADY_EXISTS);
    }

    ListInit(&Disk->Partitions);
    ListInit(&Disk->LogicalPartitions);
    Disk->PartitionCount = 0;
    Disk->LogicalCount = 0;
    Disk->PartScheme = DISK_PART_SCHEME_NONE;

    Disk->Alias[0] = '\0';
    Disk->CfgId[0] = '\0';
    Disk->CfgValid = FALSE;
    Disk->CfgStale = FALSE;
    Disk->IsRoot = FALSE;
    Disk->CfgFsKind = DISKCFG_FS_NONE;

    ListAddTail(&GDiskList, &Disk->Node);
    GDiskCount++;

    if (Disk->Read && Disk->Type != DISK_TYPE_OPTICAL) {
        PartitionScanLocked(Disk);
    }

    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "diskmgr: registered %s (%lluMB, %s, %u partitions)\n",
                  Disk->Name,
                  Disk->TotalSize / (1024 * 1024),
                  DiskPartSchemeToString(Disk->PartScheme),
                  Disk->PartitionCount + Disk->LogicalCount);

    SpinLockReleaseIrq(&GDiskLock, Flags);

    if (GDiskSysRoot) {
        if (!SysDeviceFind(Disk->Name)) {
            DiskMgrRegisterOneDisk(Disk);
        }
    }

    RETURN(SUCCESS);
}

INT DiskMgrUnregisterDisk(Disk *Dsk) {
    if (!Dsk) return NO_OBJECT;
    if (!GInitialized) return NO_OBJECT;

    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);

    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, &GDiskList) {
        Disk *D = ListEntry(Pos, Disk, Node);
        if (D == Dsk) {
            DiskClearPartitionsLocked(D);
            ListDel(&D->Node);
            GDiskCount--;
            SpinLockReleaseIrq(&GDiskLock, Flags);

            if (GDiskSysRoot) {
                SysDevice *Sd = SysDeviceFind(Dsk->Name);
                if (Sd) SysDeviceDestroy(Sd);
            }

            return SUCCESS;
        }
    }

    SpinLockReleaseIrq(&GDiskLock, Flags);
    return NOT_FOUND;
}

/* ============================================================================
 * SATA / NVMe / RAM disk init
 * ============================================================================ */

INT DiskMgrInitSata(VOID) {
    INT Result = SataInit();
    if (Result != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "diskmgr: SATA init failed (error %d)\n", Result);
        return Result;
    }

    INT Registered = 0;
    for (INT I = 0; I < 32; I++) {
        SataPort *Port = SataGetPort(I);
        if (Port && Port->Status == SATA_PORT_ACTIVE && Port->TotalSectors > 0) {
            SataRegisterDisk(Port, I);
            Registered++;
        }
    }
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "diskmgr: %d SATA disks found\n", Registered);
    return SUCCESS;
}

VOID DiskMgrInitDisks(VOID) {
    if (!GInitialized) DiskMgrInit();

    EXTERN(PataDrive, PataDrives[4]);

    BaseconPrintf(BASECON_TYPE_NORMAL, "diskmgr: scanning disks...\n");

    INT PataFoundCount = 0;
    for (INT Ch = 0; Ch < 2; Ch++) {
        for (INT Dr = 0; Dr < 2; Dr++) {
            INT Idx = Ch * 2 + Dr;
            INT Result = PataInit(&PataDrives[Idx], (PataChannel)Ch, (UINT8)Dr);
            if (Result == SUCCESS && PataDrives[Idx].TotalSectors > 0) {
                PataFoundCount++;
            }
        }
    }
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "diskmgr: %d PATA/IDE disks found\n", PataFoundCount);

    DiskMgrInitSata();
    RamDiskInit();
    DiskMgrRegisterSysDevices();

    /* Disk.cfg is used in DiskCfgInit() — call AFTER InitFs() */
}

SysDevice *DiskMgrGetSysDevice(VOID) { return GDiskSysRoot; }

/* ============================================================================
 * SysDevice
 * ============================================================================ */

STATIC VOID DiskMgrRegisterOneDisk(Disk *Disk) {
    if (!Disk || !GDiskSysRoot) return;

    SysDevice *Sd = SysDeviceCreate(GDiskSysRoot,
                                     Disk->Name,
                                     DiskTypeToString(Disk->Type),
                                     SYSDEV_TYPE_STORAGE);
    if (!Sd) return;

    SysDeviceSetProp(Sd, "type", DiskTypeToString(Disk->Type));
    SysDeviceSetProp(Sd, "part_scheme", DiskPartSchemeToString(Disk->PartScheme));
    SysDeviceSetPropU64(Sd, "sectors", Disk->SectorCount);
    SysDeviceSetPropU32(Sd, "sector_size", Disk->SectorSize);
    SysDeviceSetPropU64(Sd, "total_size", Disk->TotalSize);
    SysDeviceSetPropU32(Sd, "partition_count", Disk->PartitionCount);
    SysDeviceSetPropU32(Sd, "logical_count", Disk->LogicalCount);

    if (Disk->Alias[0])
        SysDeviceSetProp(Sd, "alias", Disk->Alias);
    if (Disk->CfgId[0])
        SysDeviceSetProp(Sd, "cfg_id", Disk->CfgId);
    SysDeviceSetPropU32(Sd, "is_root", Disk->IsRoot ? 1 : 0);

    KDriver *Drv = NULLPTR;
    switch (Disk->Type) {
        case DISK_TYPE_PATA: Drv = KDriverFindByName("PataDisk"); break;
        case DISK_TYPE_SATA: Drv = KDriverFindByName("SataDisk"); break;
        case DISK_TYPE_NVME: Drv = KDriverFindByName("Nvme");     break;
        case DISK_TYPE_RAM:  Drv = KDriverFindByName("RamDisk");  break;
        default:             Drv = NULLPTR; break;
    }

    if (Drv) SysDeviceBindDriver(Sd, Drv);
    else     SysDeviceSetState(Sd, SYSDEV_STATE_ENABLED);

    UINTPTR Flags = SpinLockAcquireIrq(&GDiskLock);

    ListHead *Pos;
    ListForEach(Pos, &Disk->Partitions) {
        Partition *Part = ListEntry(Pos, Partition, Node);
        SpinLockReleaseIrq(&GDiskLock, Flags);

        SysDevice *Psd = SysDeviceCreate(Sd, Part->Name,
                                          PartitionTypeToString(Part->Type),
                                          SYSDEV_TYPE_STORAGE);
        if (Psd) {
            SysDeviceSetProp(Psd, "partition_type",
                             PartitionTypeToString(Part->Type));
            SysDeviceSetProp(Psd, "partition_id",
                             PartitionIdToString(Part->PartitionId));
            SysDeviceSetPropHex(Psd, "start_lba", Part->StartLba);
            SysDeviceSetPropU64(Psd, "sectors", Part->SectorCount);
            SysDeviceSetPropU32(Psd, "sector_size", Part->SectorSize);
            SysDeviceSetPropU64(Psd, "size_bytes", Part->SizeInBytes);
            SysDeviceSetPropU32(Psd, "bootable", Part->Bootable ? 1 : 0);
            SysDeviceSetPropU32(Psd, "is_gpt", Part->IsGpt ? 1 : 0);
            if (Part->Label[0]) {
                SysDeviceSetProp(Psd, "label", Part->Label);
            }
            if (Part->Alias[0])
                SysDeviceSetProp(Psd, "alias", Part->Alias);
            SysDeviceSetPropU32(Psd, "is_root", Part->IsRoot ? 1 : 0);
            SysDeviceSetState(Psd, SYSDEV_STATE_ENABLED);
        }

        Flags = SpinLockAcquireIrq(&GDiskLock);
    }

    ListForEach(Pos, &Disk->LogicalPartitions) {
        Partition *Part = ListEntry(Pos, Partition, Node);
        SpinLockReleaseIrq(&GDiskLock, Flags);

        SysDevice *Psd = SysDeviceCreate(Sd, Part->Name,
                                          PartitionTypeToString(Part->Type),
                                          SYSDEV_TYPE_STORAGE);
        if (Psd) {
            SysDeviceSetProp(Psd, "partition_type",
                             PartitionTypeToString(Part->Type));
            SysDeviceSetProp(Psd, "logical", "1");
            SysDeviceSetPropHex(Psd, "start_lba", Part->StartLba);
            SysDeviceSetPropU64(Psd, "sectors", Part->SectorCount);
            SysDeviceSetPropU64(Psd, "size_bytes", Part->SizeInBytes);
            if (Part->Alias[0])
                SysDeviceSetProp(Psd, "alias", Part->Alias);
            SysDeviceSetPropU32(Psd, "is_root", Part->IsRoot ? 1 : 0);
            SysDeviceSetState(Psd, SYSDEV_STATE_ENABLED);
        }

        Flags = SpinLockAcquireIrq(&GDiskLock);
    }

    SpinLockReleaseIrq(&GDiskLock, Flags);
}

UINT32 DiskMgrRegisterSysDevices(VOID) {
    if (GDiskSysRoot) {
        UINT32 Count = 0;
        Disk *D = DiskMgrGetFirst();
        while (D) {
            if (!SysDeviceFind(D->Name)) {
                DiskMgrRegisterOneDisk(D);
                Count++;
            }
            D = DiskMgrGetNext(D);
        }
        return Count;
    }

    if (!SysDeviceGetRoot()) {
        BaseconPrintf(BASECON_TYPE_WARNING, "diskmgr: SysDevice not ready\n");
        return 0;
    }

    GDiskSysRoot = SysDeviceCreate(SysDeviceGetRoot(),
                                    "DRIVE",
                                    "Storage devices",
                                    SYSDEV_TYPE_BUS);
    if (!GDiskSysRoot) return 0;

    SysDeviceSetPropU32(GDiskSysRoot, "disk_count", GDiskCount);
    SysDeviceSetState(GDiskSysRoot, SYSDEV_STATE_ENABLED);

    UINT32 Count = 0;
    Disk *D = DiskMgrGetFirst();
    while (D) {
        DiskMgrRegisterOneDisk(D);
        Count++;
        D = DiskMgrGetNext(D);
    }
    return Count;
}