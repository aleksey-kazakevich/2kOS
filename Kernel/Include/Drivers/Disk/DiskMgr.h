#pragma once

#include <Types.h>
#include <List.h>

typedef enum {
    DISK_TYPE_PATA,
    DISK_TYPE_SATA,
    DISK_TYPE_NVME,
    DISK_TYPE_USB,
    DISK_TYPE_UNKNOWN
} DiskType;

typedef enum {
    PARTITION_TYPE_UNKNOWN,
    PARTITION_TYPE_FAT32,
    PARTITION_TYPE_FAT16,
    PARTITION_TYPE_NTFS,
    PARTITION_TYPE_EXT2,
    PARTITION_TYPE_EXT3,
    PARTITION_TYPE_EXT4,
    PARTITION_TYPE_LINUX_SWAP,
    PARTITION_TYPE_EFI_SYSTEM,
    PARTITION_TYPE_FREE,
    PARTITION_TYPE_EXTENDED
} PartitionType;

typedef struct Partition {
    ListHead Node;
    
    // --- Name ---
    CHAR Name[32];              // For example: "IDE0:2"
    UINT32 Index;               // 1-based for user
    
    // --- Physical parameters ---
    UINT64 StartLba;
    UINT64 SectorCount;
    UINT32 SectorSize;
    UINT64 SizeInBytes;
    
    // --- Тип ---
    UINT8 PartitionId;          // 0x07=NTFS, 0x0B=FAT32, 0x83=Linux
    PartitionType Type;
    
    // --- Флаги ---
    BOOL Bootable;
    BOOL Mounted;
    
    // --- Parent disk ---
    struct Disk *ParentDisk;
    
    // --- For extended partitions (logical drives) ---
    struct Partition *NextLogical;
    UINT64 ExtendedStartLba;
} Partition;

typedef struct Disk {
    ListHead Node;
    
    // --- Name ---
    CHAR Name[32];              // "IDE0", "SATA1"
    DiskType Type;
    UINT32 Index;
    
    // --- Physical parameters ---
    UINT64 SectorCount;
    UINT32 SectorSize;
    UINT64 TotalSize;
    
    // --- Partitions ---
    ListHead Partitions;        // Primary partitions
    UINT32 PartitionCount;
    ListHead LogicalPartitions; // Logical partitions
    UINT32 LogicalCount;
    
    // --- Driver data ---
    VOID *DriverData;
    BOOL Initialized;
    
    // --- Driver functions ---
    INT (*Read)(struct Disk *Disk, UINT64 Lba, UINT32 Count, VOID *Buffer);
    INT (*Write)(struct Disk *Disk, UINT64 Lba, UINT32 Count, const VOID *Buffer);
    INT (*Flush)(struct Disk *Disk);
} Disk;

// --- Initialization ---
VOID DiskMgrInit(VOID);

// --- Disc registration ---
INT DiskMgrRegisterDisk(Disk *Disk);

// --- Search ---
Disk* DiskMgrFindDisk(const CHAR *Name);
Partition* DiskMgrFindPartition(const CHAR *Name);

// --- Список дисков ---
UINT32 DiskMgrGetDiskCount(VOID);
Disk* DiskMgrGetDisk(UINT32 Index);
Disk* DiskMgrGetFirst(VOID);
Disk* DiskMgrGetNext(Disk *Prev);

// --- List of sections ---
UINT32 DiskMgrGetPartitionCount(Disk *Disk);
Partition* DiskMgrGetPartition(Disk *Disk, UINT32 Index);
Partition* DiskMgrGetPartitionByName(Disk *Disk, const CHAR *Name);

// --- Logical partitions ---
UINT32 DiskMgrGetLogicalCount(Disk *Disk);
Partition* DiskMgrGetLogical(Disk *Disk, UINT32 Index);

// --- Read/write via partition ---
INT PartitionRead(Partition *Part, UINT64 Offset, UINT32 Count, VOID *Buffer);
INT PartitionWrite(Partition *Part, UINT64 Offset, UINT32 Count, const VOID *Buffer);
INT PartitionFlush(Partition *Part);
UINT64 PartitionGetSize(Partition *Part);

// --- Read/write via name ---
INT DiskMgrRead(const CHAR *Name, UINT64 Lba, UINT32 Count, VOID *Buffer);
INT DiskMgrWrite(const CHAR *Name, UINT64 Lba, UINT32 Count, const VOID *Buffer);

INT PartitionScanMBR(Disk *Disk);

INT PartitionCreateMBR(Disk *Disk, UINT64 StartLba, UINT64 SectorCount, UINT8 PartitionId);
INT PartitionDeleteMBR(Disk *Disk, UINT32 PartitionIndex);

const CHAR* DiskTypeToString(DiskType Type);
const CHAR* PartitionTypeToString(PartitionType Type);
const CHAR* PartitionIdToString(UINT8 Id);

// Create disk name
VOID DiskMakeName(Disk *Disk);
VOID DiskMgrInitDisks(VOID);
