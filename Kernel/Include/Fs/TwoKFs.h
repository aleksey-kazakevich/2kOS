#pragma once

#include <Drivers/Disk/DiskMgr.h>
#include <Fs/Vfs.h>
#include <Types.h>

/* ============================================================================
 * 2kFS — on-disk format (v1)
 *
 * Volume layout (in 4096-byte blocks):
 *
 *   Block 0            : superblock #0
 *   Block 1            : superblock #1 (backup)
 *   Blocks 2..J+1      : journal (WAL), J blocks
 *   Blocks J+2..       : inode B-tree root + inode table blocks
 *   ...                : data blocks (extent tree, directory B-trees)
 *
 * Everything is block-aligned. All integers are little-endian.
 * Every metadata block ends with a CRC32C over its payload.
 * ========================================================================== */

#define TWOKFS_MAGIC            0x32465332U   /* "2FS2" */
#define TWOKFS_VERSION_MAJOR    1
#define TWOKFS_VERSION_MINOR    0

#define TWOKFS_BLOCK_SIZE       4096
#define TWOKFS_BLOCK_SHIFT      12
#define TWOKFS_SECTOR_SIZE      512
#define TWOKFS_SECTORS_PER_BLOCK (TWOKFS_BLOCK_SIZE / TWOKFS_SECTOR_SIZE)

#define TWOKFS_ROOT_INODE       1ULL
#define TWOKFS_NAME_MAX         255
#define TWOKFS_INODE_SIZE       256
#define TWOKFS_SYMLINK_MAX      1024

/* ----------------------------------------------------------------------------
 * Superblock (block 0)
 * -------------------------------------------------------------------------- */

typedef struct {
    UINT32 Magic;
    UINT16 VersionMajor;
    UINT16 VersionMinor;
    UINT32 BlockSize;
    UINT32 Flags;
    UINT64 TotalBlocks;
    UINT64 FreeBlocks;
    UINT64 RootInode;
    UINT64 NextIno;
    UINT64 JournalStart;          /* block number of journal[0]           */
    UINT32 JournalBlocks;         /* size of journal in blocks            */
    UINT64 InodeTreeRoot;         /* block number of inode B-tree root    */
    UINT64 BitmapStart;           /* first block of free bitmap           */
    UINT32 BitmapBlocks;
    UINT64 Uuid[2];
    UINT64 CreatedAt;
    UINT64 LastMounted;
    UINT64 LastWritten;
    UINT8  VolumeLabel[64];
    UINT8  Reserved[324];
    UINT32 Checksum;              /* CRC32C of the first 508 bytes        */
} ATTRIBUTE(packed) TwoKFsSuperblock;

_Static_assert(sizeof(TwoKFsSuperblock) == 512, "superblock == 512 bytes");

#define TWOKFS_FS_CLEAN          (1u << 0)
#define TWOKFS_FS_NEEDS_RECOVERY (1u << 1)
#define TWOKFS_FS_READ_ONLY      (1u << 2)

/* ----------------------------------------------------------------------------
 * Inode
 *
 * 256 bytes, packed. The extent tree root lives inside the inode for the
 * first few extents; when the tree outgrows it, ExtentRoot points to a
 * block-based B+-tree node.
 * -------------------------------------------------------------------------- */

#define TWOKFS_INODE_MAGIC      0x494E4F44U   /* "INOD" */

typedef struct {
    UINT64 FileOffset;            /* logical offset in bytes              */
    UINT64 DiskBlock;             /* first block on disk (0 = HOLE)       */
    UINT32 BlockCount;            /* number of blocks                     */
    UINT32 Flags;                 /* TWOKFS_EXTENT_*                      */
    UINT32 Checksum;              /* optional, if data_checksum == 1      */
    UINT32 Reserved;
} ATTRIBUTE(packed) TwoKFsExtent;

_Static_assert(sizeof(TwoKFsExtent) == 32, "extent == 32 bytes");

#define TWOKFS_EXTENT_HOLE      (1u << 0)
#define TWOKFS_EXTENT_ENCRYPTED (1u << 1)
#define TWOKFS_EXTENT_COMPRESS  (1u << 2)

#define TWOKFS_INLINE_EXTENTS   2

typedef struct {
    UINT32 Magic;
    UINT16 Mode;
    UINT16 Links;
    UINT32 Uid;
    UINT32 Gid;
    UINT64 Size;
    UINT64 Blocks;
    UINT64 Atime;
    UINT64 Mtime;
    UINT64 Ctime;
    UINT32 Flags;
    UINT32 InlineExtentCount;
    TwoKFsExtent InlineExtents[TWOKFS_INLINE_EXTENTS];
    UINT64 ExtentRoot;
    UINT32 Checksum;
    UINT8  Reserved[116];
} ATTRIBUTE(packed) TwoKFsInode;

_Static_assert(sizeof(TwoKFsInode) == 256, "inode == 256 bytes");

#define TWOKFS_INODE_IMMUTABLE  (1u << 0)
#define TWOKFS_INODE_APPEND     (1u << 1)

/* ----------------------------------------------------------------------------
 * B+-tree (generic on-disk structure)
 *
 * A B+-tree is stored as a chain of blocks. Every node starts with
 * TwoKFsBTreeNode. Leaf nodes contain keys and values; internal nodes
 * contain separator keys and child block pointers.
 * -------------------------------------------------------------------------- */

#define TWOKFS_BTREE_MAGIC      0x42545245U   /* "BTRE" */
#define TWOKFS_BTREE_LEAF       (1u << 0)
#define TWOKFS_BTREE_ROOT       (1u << 1)

#define TWOKFS_BTREE_MAX_KEYS   64            /* tune to block size          */

typedef struct {
    UINT32 Magic;
    UINT16 Level;                 /* 0 = leaf                            */
    UINT16 Flags;
    UINT32 Count;                 /* number of keys                      */
    UINT32 Reserved;
    UINT32 Checksum;
} ATTRIBUTE(packed) TwoKFsBTreeNodeHeader;

/* Directory key: hash + name.
 *   Hash = FNV-1a 64 of the UTF-8 name.
 *   Name is compared byte-by-byte only if hashes are equal. */

typedef struct {
    UINT64 Hash;
    UINT32 NameLen;
    UINT8  Name[TWOKFS_NAME_MAX];
} ATTRIBUTE(packed) TwoKFsDirKey;

typedef struct {
    UINT64 Ino;
    UINT8  Type;                  /* 1=file, 2=dir, 3=symlink            */
    UINT8  Reserved[7];
} ATTRIBUTE(packed) TwoKFsDirValue;

/* Extent key: logical offset (block-aligned). */
typedef struct {
    UINT64 FileOffset;
} ATTRIBUTE(packed) TwoKFsExtentKey;

/* ----------------------------------------------------------------------------
 * Journal (WAL)
 *
 * Ring buffer of transactions. Each transaction is:
 *
 *   TwoKFsJournalHeader (BEGIN)
 *   TwoKFsJournalRecord[]      (BlockWrite / InodeUpdate / DirInsert / DirDelete)
 *   TwoKFsJournalHeader (COMMIT)
 *
 * On recovery, transactions with COMMIT are replayed; those without are
 * ignored. After recovery, the journal is reset.
 * -------------------------------------------------------------------------- */

#define TWOKFS_JOURNAL_MAGIC    0x4A524E4CU   /* "JRNL" */

typedef enum {
    TWOKFS_JOP_BEGIN    = 1,
    TWOKFS_JOP_COMMIT   = 2,
    TWOKFS_JOP_ABORT    = 3,
    TWOKFS_JOP_CHECKPT  = 4,
    TWOKFS_JOP_BLOCK    = 0x10,
    TWOKFS_JOP_INODE    = 0x11,
    TWOKFS_JOP_DIR_ADD  = 0x12,
    TWOKFS_JOP_DIR_DEL  = 0x13,
    TWOKFS_JOP_BITMAP   = 0x14,
} TwoKFsJournalOp;

typedef struct {
    UINT32 Magic;
    UINT32 Seq;
    UINT32 Op;
    UINT32 PayloadLen;
    UINT32 Checksum;              /* CRC32C of (this header + payload)   */
    UINT32 Reserved;
} ATTRIBUTE(packed) TwoKFsJournalHeader;

typedef struct {
    UINT64 BlockNo;
    UINT8  Data[TWOKFS_BLOCK_SIZE];
} ATTRIBUTE(packed) TwoKFsJBlock;

typedef struct {
    UINT64 Ino;
    TwoKFsInode Inode;
} ATTRIBUTE(packed) TwoKFsJInode;

typedef struct {
    UINT64 DirIno;
    TwoKFsDirKey Key;
    TwoKFsDirValue Value;
} ATTRIBUTE(packed) TwoKFsJDirAdd;

typedef struct {
    UINT64 DirIno;
    TwoKFsDirKey Key;
} ATTRIBUTE(packed) TwoKFsJDirDel;

typedef struct {
    UINT64 StartBlock;
    UINT32 Count;
    UINT8  Allocated;             /* 1 = mark allocated, 0 = free        */
} ATTRIBUTE(packed) TwoKFsJBitmap;

/* ----------------------------------------------------------------------------
 * Runtime context
 * -------------------------------------------------------------------------- */

typedef struct TwoKFsContext {
    Disk          *Disk;
    UINT64         StartLba;
    UINT64         SectorCount;
    TwoKFsSuperblock Sb;
    UINT8         *Bitmap;
    UINT32         BitmapBytes;
    UINT64         JournalWrite;      /* next free journal slot          */
    UINT32         JournalSeq;
    struct VfsInode *MountRoot;
} TwoKFsContext;

typedef struct {
    TwoKFsContext *Ctx;
    UINT64         Ino;
    UINT64         ParentIno;
} TwoKFsPrivate;

/* ----------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

VOID TwoKFsInit(VOID);
INT  TwoKFsFormat(Disk *Disk, UINT64 StartLba, UINT64 SectorCount);

/* Low-level helpers */
UINT32 TwoKFsCrc32c(const VOID *Data, USIZE Len);
INT    TwoKFsReadSuperblock(Disk *Disk, UINT64 StartLba, TwoKFsSuperblock *Out);
INT    TwoKFsWriteSuperblock(Disk *Disk, UINT64 StartLba,
                             const TwoKFsSuperblock *Sb);