#pragma once

#include <Drivers/Disk/DiskMgr.h>
#include <Fs/Vfs.h>

/* exFAT signatures */
#define EXFAT_SIGNATURE     0x7C8E4FD2A5C14F88

/* FAT values */
#define EXFAT_FAT_FREE      0x00000000
#define EXFAT_FAT_END       0xFFFFFFFF
#define EXFAT_FAT_BAD       0xFFFFFFF7

/* Types of directory entries */
#define EXFAT_ENTRY_FILE        0x85
#define EXFAT_ENTRY_VOLUME      0x83
#define EXFAT_ENTRY_BITMAP      0x81
#define EXFAT_ENTRY_UPCASE      0x82
#define EXFAT_ENTRY_NAME        0xC1
#define EXFAT_ENTRY_STREAM      0xC0

/* File attributes */
#define EXFAT_ATTR_READ_ONLY    0x01
#define EXFAT_ATTR_HIDDEN       0x02
#define EXFAT_ATTR_SYSTEM       0x04
#define EXFAT_ATTR_VOLUME_ID    0x08
#define EXFAT_ATTR_DIRECTORY    0x10
#define EXFAT_ATTR_ARCHIVE      0x20

/* VBR (Volume Boot Record) */
typedef struct {
    UINT8  JumpBoot[3];
    UINT8  FsName[8];
    UINT8  Reserved[53];
    UINT64 PartitionOffset;
    UINT64 VolumeLength;
    UINT32 FatOffset;
    UINT32 FatLength;
    UINT32 ClusterHeapOffset;
    UINT32 ClusterCount;
    UINT32 RootDirCluster;
    UINT32 VolumeSerial;
    UINT16 FsRevision;
    UINT16 VolumeFlags;
    UINT8  BytesPerSectorShift;
    UINT8  SectorsPerClusterShift;
    UINT8  NumberOfFats;
    UINT8  DiskSelect;
    UINT8  PercentInUse;
    UINT8  Reserved2[7];
    UINT8  BootCode[390];
    UINT16 Signature;
} ATTRIBUTE(packed) ExfatVbr;

/* File directory entry */
typedef struct {
    UINT8  Type;
    UINT8  SecondaryCount;
    UINT16 Checksum;
    UINT16 FileAttributes;
    UINT16 Reserved1;
    UINT32 CreateTime;
    UINT32 ModifyTime;
    UINT32 AccessTime;
    UINT8  CreateTime10ms;
    UINT8  ModifyTime10ms;
    UINT8  CreateTzOffset;
    UINT8  ModifyTzOffset;
    UINT8  AccessTzOffset;
    UINT8  Reserved2[7];
} ATTRIBUTE(packed) ExfatFileEntry;

/* Stream entry */
typedef struct {
    UINT8  Type;
    UINT8  Flags;
    UINT8  Reserved1;
    UINT8  NameLength;
    UINT16 NameHash;
    UINT16 Reserved2;
    UINT64 ValidDataLength;
    UINT32 Reserved3;
    UINT32 FirstCluster;
    UINT64 DataLength;
} ATTRIBUTE(packed) ExfatStreamEntry;

/* Name entry (UTF-16) */
typedef struct {
    UINT8  Type;
    UINT8  Flags;
    UINT16 Name[15];
} ATTRIBUTE(packed) ExfatNameEntry;

/* exFAT context */
typedef struct {
    Disk       *Disk;
    UINT64      StartLba;
    UINT64      SectorCount;
    ExfatVbr    Vbr;
    UINT32      BytesPerSector;
    UINT32      SectorsPerCluster;
    UINT32      BytesPerCluster;
    UINT64      FatStart;
    UINT64      ClusterHeapStart;
    UINT32      RootCluster;
    UINT32      *FatCache;
    UINT32      FatEntries;
    struct VfsInode *MountRoot;
} ExfatContext;

/* Private inode data */
typedef struct {
    ExfatContext *Exfat;
    UINT32       FirstCluster;
    UINT64       DataLength;
    UINT32       DirCluster;
    UINT32       DirEntry;
    UINT32       ParentCluster;
    struct VfsInode *ParentInode;
} ExfatPrivate;

/* Initialization */
INT ExfatInit(VOID);
/* Formatting */
INT ExfatFormat(Disk *Disk, UINT64 StartLba, UINT64 SectorCount);