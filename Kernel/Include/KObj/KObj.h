#pragma once

#include <SpinLock.h>
#include <Types.h>
#include <List.h>

#define KOBJ_NAME_MAX       128
#define KOBJ_PATH_MAX       256
#define KOBJ_HANDLE_MAX     64   /* per-task */

typedef enum {
    KOBJ_TYPE_UNKNOWN = 0,
    KOBJ_TYPE_DIRECTORY,
    KOBJ_TYPE_STDIO,
    KOBJ_TYPE_BUFFER,       /* reserved for the future */
    KOBJ_TYPE_EVENT,        /* reserved for the future */
} KObjType;

typedef enum {
    KOBJ_ACCESS_READ  = 1,
    KOBJ_ACCESS_WRITE = 2,
    KOBJ_ACCESS_BOTH  = 3,
} KObjAccess;

struct KObj;

typedef struct KObjOperations {
    INT  (*Read)(struct KObj *Obj, UINT64 Offset, VOID *Buf, UINT32 Size, UINT32 *Read);
    INT  (*Write)(struct KObj *Obj, UINT64 Offset, const VOID *Buf, UINT32 Size, UINT32 *Written);
    INT  (*Ioctl)(struct KObj *Obj, UINT32 Cmd, VOID *Arg);
    VOID (*Close)(struct KObj *Obj);
    VOID (*Destroy)(struct KObj *Obj);
} KObjOperations;

typedef struct KObj {
    ListHead Node;              /* global list (unused for now) */
    ListHead ChildNode;         /* in parent's Children */

    CHAR   Name[KOBJ_NAME_MAX];
    CHAR   FullPath[KOBJ_PATH_MAX];

    KObjType Type;
    UINT32   Access;

    KObjOperations *Ops;
    VOID   *Private;
    volatile UINT32 RefCount;

    struct KObj *Parent;
    ListHead Children;
    UINT32   ChildCount;

    SpinLock Lock;              /* protects internal state */
} KObj;

typedef struct {
    BOOL  Valid;
    KObj *Object;
    UINT32 Access;
} KObjHandle;

/* --- Lifecycle --- */
VOID  KObjInit(VOID);
KObj *KObjGetRoot(VOID);

/* --- Registration --- */
KObj *KObjCreate(KObj *Parent, const CHAR *Name, KObjType Type,
                 KObjOperations *Ops, VOID *Private);
VOID  KObjDestroy(KObj *Obj);   /* forcibly destroys with all children */

/* --- Lookup --- */
KObj *KObjFindByPath(const CHAR *Path);
KObj *KObjFindByPathRelative(KObj *Base, const CHAR *Path);

/* --- Refcount --- */
VOID KObjRef(KObj *Obj);
VOID KObjUnref(KObj *Obj);

/* --- Directory listing --- */
INT KObjReadDir(KObj *Dir, UINT64 *Pos, CHAR *Name, UINT32 *NameLen, KObjType *Type);

/* --- Per-task handle helpers --- */
KObj *KObjHandleGet(UINT32 Handle, UINT32 RequiredAccess);
INT   KObjHandleInstall(KObj *Obj, UINT32 Access);   /* consumes the caller's ref */
INT   KObjHandleClose(UINT32 Handle);