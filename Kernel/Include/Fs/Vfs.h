#pragma once

#include <Drivers/Disk/DiskMgr.h>
#include <Types.h>
#include <List.h>

// ============================================================================
// CONSTANTS
// ============================================================================

// File types
#define FT_UNKNOWN   0
#define FT_REG_FILE  1
#define FT_DIR       2
#define FT_CHRDEV    3
#define FT_BLKDEV    4
#define FT_FIFO      5
#define FT_SOCK      6
#define FT_SYMLINK   7

// Opening flags
#define O_READ       1
#define O_WRITE      2
#define O_CREAT      4
#define O_TRUNC      8
#define O_APPEND     16

#define PATH_MAX     4096
#define NAME_MAX     255

#define SEEK_SET     0
#define SEEK_CUR     1
#define SEEK_END     2

// ============================================================================
// STRUCTURES
// ============================================================================

// Forward declarations
struct VfsInode;
struct VfsFile;

// Operations with inodes (metadata)
typedef struct VfsOperations {
    INT (*Lookup)(struct VfsInode *Dir, const CHAR *Name, struct VfsInode **Result);
    INT (*Create)(struct VfsInode *Dir, const CHAR *Name, UINT32 Mode, struct VfsInode **Result);
    INT (*Unlink)(struct VfsInode *Dir, const CHAR *Name);
    INT (*Mkdir)(struct VfsInode *Dir, const CHAR *Name, UINT32 Mode, struct VfsInode **Result);
    INT (*Rmdir)(struct VfsInode *Dir, const CHAR *Name);
    INT (*Rename)(struct VfsInode *OldDir, const CHAR *OldName, 
                  struct VfsInode *NewDir, const CHAR *NewName);
    INT (*Chmod)(struct VfsInode *Inode, UINT32 Mode);
    INT (*Stat)(struct VfsInode *Inode, VOID *StatBuf);
    INT (*ReadDir)(struct VfsInode *Dir, UINT64 *Pos, CHAR *Name, 
                   UINT32 *NameLen, UINT32 *Type);
    INT (*Parent)(struct VfsInode *Inode, struct VfsInode **Parent);
    INT (*GetName)(struct VfsInode *Inode, CHAR *Name, INT MaxLen);
    INT (*Unmount)(struct VfsInode *Root);
    VOID (*Destroy)(struct VfsInode *Inode);
} VfsOperations;

// Operations with open files (data)
typedef struct VfsFileOperations {
    INT (*Read)(struct VfsInode *Inode, UINT64 Offset, VOID *Buf, UINT32 Size, UINT32 *Read);
    INT (*Write)(struct VfsInode *Inode, UINT64 Offset, const VOID *Buf, UINT32 Size, UINT32 *Written);
    INT (*Truncate)(struct VfsInode *Inode, UINT64 NewSize);
    INT (*Sync)(struct VfsInode *Inode);
} VfsFileOperations;

// Inode (represents a file or directory)
typedef struct VfsInode {
    UINT32 IMode;        // Type and rights
    UINT32 IUid;
    UINT32 IGid;
    UINT64 ISize;
    UINT64 ICtime;       // Creation time
    UINT64 IMtime;       // Modify time
    UINT64 IAtime;       // Access time
    UINT64 IIno;         // Unique number

    Disk *IDev;          // The device on which the inode is located
    CHAR IFsName[16];    // File system name ("fat32", "exfat")
    
    // Links
    UINT32 INlink;       // Number of hard links
    
    // Operations
    VfsOperations *IOp;
    VfsFileOperations *IFop;

    UINT32 IRefCount;
    
    // Private data of a specific FS
    VOID *IPrivate;
    
    // For caching
    INT IDirty;
    struct VfsInode *INext;  // For list
} VfsInode;

// Open file (file descriptor)
typedef struct VfsFile {
    VfsInode *FInode;
    UINT64 FPos;
    UINT32 FFlags;
    VOID *FPrivate;        // For a specific FS
} VfsFile;

// Structure for registering a file system
typedef struct FileSystem {
    CHAR Name[16];
    INT (*Mount)(Disk *Dev, UINT64 StartLba, UINT64 SectorCount, struct VfsInode **Root);
    INT (*Unmount)(struct VfsInode *Root);
    struct FileSystem *Next;
} FileSystem;

// File statistics
typedef struct VfsStat {
    UINT32 StMode;
    UINT32 StUid;
    UINT32 StGid;
    UINT64 StSize;
    UINT64 StCtime;
    UINT64 StMtime;
    UINT64 StAtime;
    UINT64 StIno;
    UINT32 StNlink;
} VfsStats;

typedef struct {
    CHAR Name[256];
    VfsInode *Inode;
    INT Type;
} MountEntry;

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

EXTERN(VfsInode, *VfsCurrentDir);
EXTERN(VfsInode, *VfsRootInode);
EXTERN(CHAR, VfsCurrentPath[PATH_MAX]);
EXTERN(CHAR, VfsCurrentDrive[DRIVE_NAME_MAX]);

// ============================================================================
// VFS CORE
// ============================================================================

// Initialization
VOID VfsInit(VOID);

// File system registration
INT VfsRegisterFs(FileSystem *Fs);

const CHAR *VfsDetectFilesystemAt(Disk *Dev, UINT64 StartLba);
const CHAR *VfsDetectFilesystem(Disk *Dev);

// Mounting
INT VfsMount(const CHAR *FsName, Disk *Dev, UINT64 StartLba, UINT64 SectorCount, VfsInode **Root);
INT VfsUnmount(VfsInode *Root);
INT VfsUnmountDrive(const CHAR *DriveName);
INT VfsListMountedDrives(CHAR *Buf, USIZE BufSize);

// Path parsing: "SATA0::/folder/file" -> drive="SATA0", rel="/folder/file"
INT VfsParsePath(const CHAR *Path, CHAR *DriveOut, CHAR *RelOut, USIZE OutSize);

// Path resolution
INT VfsWalk(VfsInode *Dir, const CHAR *Path, VfsInode **Result);
INT VfsWalkParent(VfsInode *Dir, const CHAR *Path, VfsInode **Parent, CHAR *Name);
INT VfsLookup(VfsInode *Dir, const CHAR *Name, VfsInode **Result);
INT VfsResolvePath(VfsInode *BaseDir, const CHAR *Path, 
                   VfsInode **Parent, CHAR *Name, BOOL FollowLast);

// File operations
INT VfsOpen(VfsInode *Dir, const CHAR *Path, UINT32 Flags, VfsFile **File);
INT VfsClose(VfsFile *File);
INT VfsRead(VfsFile *File, VOID *Buf, UINT32 Size, UINT32 *Read);
INT VfsWrite(VfsFile *File, const VOID *Buf, UINT32 Size, UINT32 *Written);
INT VfsSeek(VfsFile *File, UINT64 Offset, INT Whence);

// Directory operations
INT VfsMkdir(VfsInode *Dir, const CHAR *Path, UINT32 Mode, VfsInode **Result);
INT VfsRmdir(VfsInode *Dir, const CHAR *Path);
INT VfsReadDir(VfsInode *Dir, UINT64 *Pos, CHAR *Name, UINT32 *NameLen, UINT32 *Type);

// File management
INT VfsCreate(VfsInode *Dir, const CHAR *Path, UINT32 Mode, VfsInode **Result);
INT VfsUnlink(VfsInode *Dir, const CHAR *Path);
INT VfsRename(VfsInode *OldDir, const CHAR *OldPath, 
              VfsInode *NewDir, const CHAR *NewPath);

// Metadata
INT VfsStat(VfsInode *Inode, VfsStats *Stat);
INT VfsChmod(VfsInode *Inode, UINT32 Mode);
INT VfsChown(VfsInode *Inode, UINT32 Uid, UINT32 Gid);
INT VfsSync(VfsInode *Inode);

// Inode management (for FS drivers)
VfsInode *VfsAllocInode(VOID);
VOID VfsFreeInode(VfsInode *Inode);
VOID VfsInodeRef(VfsInode *Inode);
VOID VfsInodeUnref(VfsInode *Inode);
INT VfsParent(VfsInode *Inode, VfsInode **Parent);

// Current directory
INT VfsCd(VfsInode **Dir, const CHAR *Path);
VOID VfsUpdateCurrentPath(VOID);
INT VfsBuildPath(VfsInode *Inode, CHAR *Buffer, INT MaxLen);

// Utility
INT BuildCurrentPath(VfsInode *Dir, const CHAR *Component, CHAR *Out);
INT VfsGetPathDepth(VfsInode *Inode);