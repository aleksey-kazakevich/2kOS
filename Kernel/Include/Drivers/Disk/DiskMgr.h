#pragma once

#include <SysDevice.h>
#include <Types.h>
#include <List.h>

typedef enum {
    DISK_TYPE_PATA,
    DISK_TYPE_SATA,
    DISK_TYPE_NVME,
    DISK_TYPE_USB,
    DISK_TYPE_RAM,
    DISK_TYPE_OPTICAL,
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

/* Disk layout scheme */
typedef enum {
    DISK_PART_SCHEME_NONE = 0,   /* signature not found */
    DISK_PART_SCHEME_MBR,
    DISK_PART_SCHEME_GPT,
} DiskPartScheme;

typedef enum {
    DISKCFG_FS_NONE = 0,
    DISKCFG_FS_FAT32,
    DISKCFG_FS_EXFAT,
    DISKCFG_FS_ISO9660,   // read-only, cfg is written to the image during assembly
} DiskCfgFsKind;

#define DRIVE_NAME_MAX 32

#define DISKCFG_ID_MAX       40
#define DISKCFG_NAME_MAX     32
#define DISKCFG_FILE_NAME    "disk.cfg"
#define DISKCFG_VERSION      1

typedef struct {
    BOOL   Valid;                        // successfully read/parsed
    BOOL   Stale;                        // the ID in the file did not match the real one
    UINT32 Version;                      // version=1
    CHAR   Id[DISKCFG_ID_MAX];           // persistent id
    CHAR   Name[DISKCFG_NAME_MAX];       // disk_name
    BOOL   IsRoot;                       // is_root=0/1
    DiskCfgFsKind FsKind;                // filled in by the caller
} DiskCfg;

typedef struct Partition {
    ListHead Node;

    /* --- Name --- */
    CHAR Name[DRIVE_NAME_MAX];      /* "IDE0:2" or "NVME1p1" */
    CHAR Alias[DRIVE_NAME_MAX];    // alias ("rootfs") from disk.cfg
    UINT32 Index;                   /* 1-based for the user */

    CHAR   CfgId[DISKCFG_ID_MAX];
    BOOL   CfgValid;
    BOOL   CfgStale;
    BOOL   IsRoot;
    DiskCfgFsKind CfgFsKind;

    /* --- Physical parameters --- */
    UINT64 StartLba;
    UINT64 SectorCount;
    UINT32 SectorSize;
    UINT64 SizeInBytes;

    /* --- Type --- */
    UINT8 PartitionId;              /* MBR ID (synthetic for GPT) */
    PartitionType Type;

    /* --- GPT metadata --- */
    BOOL     IsGpt;
    CHAR     Label[36];             /* UTF-8 section name (GUID entry Name) */
    UINT8    TypeGuid[16];          /* the raw GUID of the type */
    UINT8    UniqueGuid[16];        /* the raw GUID of the instance */
    UINT64   Attributes;            /* GPT Attributes */

    /* --- Flags --- */
    BOOL Bootable;
    BOOL Mounted;

    /* --- Parent disk --- */
    struct Disk *ParentDisk;

    /* --- Extended partitions (MBR only) --- */
    struct Partition *NextLogical;
    UINT64 ExtendedStartLba;
} Partition;

typedef struct Disk {
    ListHead Node;

    /* --- Name --- */
    CHAR Name[32];
    CHAR Alias[DRIVE_NAME_MAX];
    DiskType Type;
    UINT32 Index;

    CHAR   CfgId[DISKCFG_ID_MAX];  // persistent id (hex)
    BOOL   CfgValid;               // disk.cfg has been read and the id matches
    BOOL   CfgStale;               // disk.cfg is there, but the id didn't match
    BOOL   IsRoot;                 // mount at startup
    DiskCfgFsKind CfgFsKind;       // for reuse

    /* --- Physical parameters --- */
    UINT64 SectorCount;
    UINT32 SectorSize;
    UINT64 TotalSize;

    /* --- Partition scheme --- */
    DiskPartScheme PartScheme;
    UINT8 DiskGuid[16];             /* GPT DiskGUID (for GPT) */

    /* --- Partitions --- */
    ListHead Partitions;
    UINT32 PartitionCount;
    ListHead LogicalPartitions;     /* MBR extended only */
    UINT32 LogicalCount;

    /* --- Driver data --- */
    VOID *DriverData;
    BOOL Initialized;

    /* --- Driver functions --- */
    INT (*Read)(struct Disk *Disk, UINT64 Lba, UINT32 Count, VOID *Buffer);
    INT (*Write)(struct Disk *Disk, UINT64 Lba, UINT32 Count, const VOID *Buffer);
    INT (*Flush)(struct Disk *Disk);
} Disk;

/* ============================================================================ Initialization ============================================================================ */

VOID DiskMgrInit(VOID);
VOID DiskMgrInitDisks(VOID);
VOID DiskCfgInit(VOID);

/* ============================================================================ Disk registration ============================================================================ */

INT DiskMgrRegisterDisk(Disk *Disk);
INT DiskMgrUnregisterDisk(Disk *Disk);

/* ============================================================================ Search ============================================================================ */

Disk* DiskMgrFindDisk(const CHAR *Name);
Partition* DiskMgrFindPartition(const CHAR *Name);

/* ============================================================================ Disk list ============================================================================ */

UINT32 DiskMgrGetDiskCount(VOID);
Disk* DiskMgrGetDisk(UINT32 Index);
Disk* DiskMgrGetFirst(VOID);
Disk* DiskMgrGetNext(Disk *Prev);

/* ============================================================================ Partition list ============================================================================ */

UINT32 DiskMgrGetPartitionCount(Disk *Disk);
Partition* DiskMgrGetPartition(Disk *Disk, UINT32 Index);
Partition* DiskMgrGetPartitionByName(Disk *Disk, const CHAR *Name);

UINT32 DiskMgrGetLogicalCount(Disk *Disk);
Partition* DiskMgrGetLogical(Disk *Disk, UINT32 Index);

/* ============================================================================ Read / write ============================================================================ */

INT PartitionRead(Partition *Part, UINT64 Offset, UINT32 Count, VOID *Buffer);
INT PartitionWrite(Partition *Part, UINT64 Offset, UINT32 Count, const VOID *Buffer);
INT PartitionFlush(Partition *Part);
UINT64 PartitionGetSize(Partition *Part);

INT DiskMgrRead(const CHAR *Name, UINT64 Lba, UINT32 Count, VOID *Buffer);
INT DiskMgrWrite(const CHAR *Name, UINT64 Lba, UINT32 Count, const VOID *Buffer);

/* ============================================================================ Partition scan ============================================================================ */

/* Automatically detects GPT or MBR and parses the partitions. */
INT PartitionScan(Disk *Disk);

/* Analysis of a specific scheme (for explicit invocation). */
INT PartitionScanMBR(Disk *Disk);
INT PartitionScanGPT(Disk *Disk);

/* ============================================================================ MBR: create / delete ============================================================================ */

INT PartitionCreateMBR(Disk *Disk, UINT64 StartLba, UINT64 SectorCount, UINT8 PartitionId);
INT PartitionDeleteMBR(Disk *Disk, UINT32 PartitionIndex);

/* ============================================================================ GPT: create / delete ============================================================================ */

/*
   Create a GPT partition.
   Type — PartitionType (or PARTITION_TYPE_UNKNOWN for the default type).
   Label — UTF-8, up to 35 characters (cut off).
 */
INT PartitionCreateGPT(Disk *Disk, UINT64 StartLba, UINT64 SectorCount,
                       PartitionType Type, const CHAR *Label);

/* Delete the GPT partition by 1-based index (internal Partition->Index). */
INT PartitionDeleteGPT(Disk *Disk, UINT32 PartitionIndex);

/* Initialize an empty GPT on the disk (creates a protective MBR, GPT Header on LBA 1, entries on LBA 2, backup at the end). */
INT GptInitDisk(Disk *Disk);

/* ============================================================================ Utility ============================================================================ */

const CHAR* DiskTypeToString(DiskType Type);
const CHAR* PartitionTypeToString(PartitionType Type);
const CHAR* PartitionIdToString(UINT8 Id);
const CHAR* DiskPartSchemeToString(DiskPartScheme Scheme);

VOID DiskMakeName(Disk *Disk);

/* ============================================================================ SysDevice ============================================================================ */

UINT32 DiskMgrRegisterSysDevices(VOID);
SysDevice *DiskMgrGetSysDevice(VOID);

/* ============================================================================ DiskCfg ============================================================================ */

/* In-memory parsing/serialization (without I/O) */
INT  DiskCfgParse(const CHAR *Buf, USIZE Len, DiskCfg *Out);
INT  DiskCfgSerialize(const DiskCfg *Cfg, CHAR *Buf, USIZE BufSize);

/* Calculating the persistent id for a disk/partition */
VOID DiskCfgComputeId(Disk *D, UINT64 StartLba, UINT64 SectorCount,
                      DiskCfgFsKind Fs, char *Out, USIZE OutSize);

/* Read/write disk.cfg from disk (mini-parser, without VFS-mount) */
INT  DiskCfgReadVolume(Disk *D, UINT64 StartLba, UINT64 SectorCount,
                       DiskCfgFsKind Fs, DiskCfg *Out);
INT  DiskCfgWriteVolume(Disk *D, UINT64 StartLba, UINT64 SectorCount,
                        DiskCfgFsKind Fs, const CHAR *Name, BOOL IsRoot);

/* Apply cfg to disk/partition (id-matching, alias, is_root) */
INT  DiskCfgApplyToDisk(Disk *D);
INT  DiskCfgApplyToPartition(Partition *P);

/* Mounting all is_root=1 volumes (call AFTER InitFs) */
VOID DiskCfgMountRoots(VOID);

/* Utilities */
BOOL DiskCfgAliasExists(const CHAR *Alias, Disk *SelfD, Partition *SelfP);