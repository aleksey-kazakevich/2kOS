#pragma once

#include <Linux/Fcntl.h>
#include <KObj/KObj.h>
#include <Fs/Vfs.h>
#include <Types.h>

/*
 * Per-task file descriptor table.
 *
 * fd 0, 1, 2 are pre-installed at task creation and point to
 * the task's stdin/stdout/stderr KObj bindings (or the default
 * console). fd >= 3 are allocated on demand by open()/openat()
 * and may refer to either a VFS file or a KObj.
 *
 * This mirrors the Linux model: a single integer namespace,
 * per-process, with the first three slots reserved for the
 * standard streams.
 */

#define FD_MAX          256
#define FD_FIRST_USER   3

typedef enum {
    FD_KIND_FREE = 0,
    FD_KIND_VFS,        /* backed by VfsFile   */
    FD_KIND_KOBJ,       /* backed by KObj      */
} FdKind;

typedef struct {
    FdKind Kind;
    UINT32 Flags;       /* VFS_O_READ, VFS_O_WRITE, VFS_O_APPEND, ... */
    union {
        VfsFile *File;
        KObj    *Obj;
    };
    UINT64 Offset;      /* only meaningful for FD_KIND_VFS */
    UINT32 RefCount;    /* for future dup() support */
} FdEntry;

typedef struct {
    FdEntry Entries[FD_MAX];
    UINT32  Count;      /* number of open (non-FREE) entries */
} FdTable;

/* Lifecycle. */
VOID     FdTableInit(FdTable *T);
VOID     FdTableCloseAll(FdTable *T);

/* Allocate / free. */
INT      FdAlloc(FdTable *T);                       /* returns fd or -EMFILE */
INT      FdAllocFrom(FdTable *T, UINT32 MinFd);     /* for F_DUPFD-style */
VOID     FdFree(FdTable *T, UINT32 Fd);
FdEntry *FdGet(FdTable *T, UINT32 Fd);              /* NULL if invalid/free */
INT      FdInstallVfs(FdTable *T, VfsFile *File, UINT32 Flags);
INT      FdInstallKObj(FdTable *T, KObj *Obj, UINT32 Flags);

/* Accessors. */
VfsFile *FdGetVfs(FdTable *T, UINT32 Fd);
KObj    *FdGetKObj(FdTable *T, UINT32 Fd);

/* Standard streams. */
VOID     FdSetupStdio(FdTable *T, KObj *In, KObj *Out, KObj *Err);