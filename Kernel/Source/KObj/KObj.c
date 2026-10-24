#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <Scheduler.h>
#include <KObj/KObj.h>
#include <SpinLock.h>
#include <Return.h>

STATIC KObj *GObjRoot = NULLPTR;
STATIC SpinLock GObjLock;

/* ============================================================================
 * INIT
 * ============================================================================ */

VOID KObjInit(VOID) {
    SpinLockInit(&GObjLock);

    GObjRoot = (KObj*)MemoryAllocate(sizeof(KObj));
    if (!GObjRoot) {
        StdErrPrintf("kobj: cannot alloc root\n");
        return;
    }
    MemSet(GObjRoot, 0, sizeof(KObj));
    StrCpy(GObjRoot->Name, "\\");
    StrCpy(GObjRoot->FullPath, "\\");
    GObjRoot->Type = KOBJ_TYPE_DIRECTORY;
    GObjRoot->Access = KOBJ_ACCESS_READ;
    GObjRoot->RefCount = 1;
    ListInit(&GObjRoot->Children);
    SpinLockInit(&GObjRoot->Lock);

    StdOutPrintf("kobj: namespace initialized\n");
}

KObj *KObjGetRoot(VOID) { return GObjRoot; }

/* ============================================================================
 * REFCOUNT
 * ============================================================================ */

VOID KObjRef(KObj *Obj) {
    if (!Obj) return;
    __sync_fetch_and_add(&Obj->RefCount, 1);
}

VOID KObjUnref(KObj *Obj) {
    if (!Obj || Obj == GObjRoot) return;

    UINT32 Old = __sync_fetch_and_sub(&Obj->RefCount, 1);
    if (Old != 1) return;

    /* Last reference — remove from the parent's children list
     * BEFORE freeing, so that subsequent lookups don't see a
     * dangling ChildNode. */
    KObj *Parent = Obj->Parent;
    if (Parent) {
        UINTPTR F = SpinLockAcquireIrq(&GObjLock);
        ListDel(&Obj->ChildNode);
        if (Parent->ChildCount > 0) Parent->ChildCount--;
        Parent = NULLPTR;
        SpinLockReleaseIrq(&GObjLock, F);
    }

    if (Obj->Ops && Obj->Ops->Destroy) Obj->Ops->Destroy(Obj);
    if (Obj->Private) { MemoryFree(Obj->Private); Obj->Private = NULLPTR; }
    MemoryFree(Obj);

    if (Parent) KObjUnref(Parent);
}

/* ============================================================================
 * PATH BUILDER
 * ============================================================================ */

STATIC VOID KObjBuildPath(KObj *Obj, CHAR *Out, USIZE OutSz) {
    KObj *Stack[16];
    INT Depth = 0;
    for (KObj *P = Obj; P && P != GObjRoot && Depth < 16; P = P->Parent) {
        Stack[Depth++] = P;
    }

    INT Pos = 0;
    Out[Pos++] = '\\';
    for (INT I = Depth - 1; I >= 0; I--) {
        USIZE L = StrLen(Stack[I]->Name);
        if (Pos + L + 2 >= (INT)OutSz) break;
        for (USIZE K = 0; K < L; K++) Out[Pos++] = Stack[I]->Name[K];
        if (I > 0) Out[Pos++] = '\\';
    }
    Out[Pos] = '\0';
}

/* ============================================================================
 * CREATE
 * ============================================================================ */

KObj *KObjCreate(KObj *Parent, const CHAR *Name, KObjType Type,
                 KObjOperations *Ops, VOID *Private) {
    if (!Parent || !Name || !Name[0]) return NULLPTR;
    if (Parent->Type != KOBJ_TYPE_DIRECTORY) return NULLPTR;

    UINTPTR F = SpinLockAcquireIrq(&GObjLock);

    /* Duplicate check */
    ListHead *Pos;
    ListForEach(Pos, &Parent->Children) {
        KObj *C = ListEntry(Pos, KObj, ChildNode);
        if (StrCaseCmp(C->Name, Name) == 0) {
            SpinLockReleaseIrq(&GObjLock, F);
            return NULLPTR;
        }
    }

    KObj *Obj = (KObj*)MemoryAllocate(sizeof(KObj));
    if (!Obj) {
        SpinLockReleaseIrq(&GObjLock, F);
        return NULLPTR;
    }
    MemSet(Obj, 0, sizeof(KObj));

    StrnCpy(Obj->Name, Name, KOBJ_NAME_MAX - 1);
    Obj->Type    = Type;
    Obj->Ops     = Ops;
    Obj->Private = Private;
    Obj->Parent  = Parent;
    Obj->RefCount = 1;
    Obj->Access  = KOBJ_ACCESS_BOTH;
    ListInit(&Obj->Children);
    SpinLockInit(&Obj->Lock);

    ListAddTail(&Parent->Children, &Obj->ChildNode);
    Parent->ChildCount++;
    KObjRef(Parent);

    KObjBuildPath(Obj, Obj->FullPath, KOBJ_PATH_MAX);

    SpinLockReleaseIrq(&GObjLock, F);
    return Obj;
}

/* ============================================================================
 * FORCED DESTROY (removes from the tree and kills all children)
 * ============================================================================ */

STATIC VOID KObjDestroyRecursive(KObj *Obj) {
    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, &Obj->Children) {
        KObj *C = ListEntry(Pos, KObj, ChildNode);
        ListDel(&C->ChildNode);
        KObjDestroyRecursive(C);
    }
    KObjUnref(Obj);
}

VOID KObjDestroy(KObj *Obj) {
    if (!Obj || Obj == GObjRoot) return;
    UINTPTR F = SpinLockAcquireIrq(&GObjLock);
    KObjDestroyRecursive(Obj);
    SpinLockReleaseIrq(&GObjLock, F);
}

/* ============================================================================
 * LOOKUP
 * ============================================================================ */

STATIC KObj *KObjFindChildLocked(KObj *Parent, const CHAR *Name) {
    ListHead *Pos;
    ListForEach(Pos, &Parent->Children) {
        KObj *C = ListEntry(Pos, KObj, ChildNode);
        if (StrCaseCmp(C->Name, Name) == 0) return C;
    }
    return NULLPTR;
}

KObj *KObjFindByPathRelative(KObj *Base, const CHAR *Path) {
    if (!Path) return NULLPTR;
    if (!Base) Base = GObjRoot;

    UINTPTR F = SpinLockAcquireIrq(&GObjLock);

    CHAR Buf[KOBJ_PATH_MAX];
    StrnCpy(Buf, Path, KOBJ_PATH_MAX - 1);
    Buf[KOBJ_PATH_MAX - 1] = '\0';

    KObj *Cur = Base;
    if (Buf[0] == '\\' || Buf[0] == '/') Cur = GObjRoot;

    CHAR *Save = NULLPTR;
    CHAR *Tok = StrTokR(Buf, "\\/", &Save);
    while (Tok) {
        if (StrCmp(Tok, ".") == 0) {
            /* skip */
        } else if (StrCmp(Tok, "..") == 0) {
            if (Cur->Parent) Cur = Cur->Parent;
        } else {
            KObj *Next = KObjFindChildLocked(Cur, Tok);
            if (!Next) { Cur = NULLPTR; break; }
            Cur = Next;
        }
        Tok = StrTokR(NULLPTR, "\\/", &Save);
    }

    if (Cur) KObjRef(Cur);
    SpinLockReleaseIrq(&GObjLock, F);
    return Cur;
}

KObj *KObjFindByPath(const CHAR *Path) {
    return KObjFindByPathRelative(GObjRoot, Path);
}

/* ============================================================================
 * READDIR
 * ============================================================================ */

INT KObjReadDir(KObj *Dir, UINT64 *Pos, CHAR *Name, UINT32 *NameLen, KObjType *Type) {
    if (!Dir || !Pos || !Name || !NameLen || !Type) return -1;
    if (Dir->Type != KOBJ_TYPE_DIRECTORY) return -1;

    UINTPTR F = SpinLockAcquireIrq(&GObjLock);

    UINT64 Idx = 0;
    INT Rc = -1;
    ListHead *P;
    ListForEach(P, &Dir->Children) {
        if (Idx == *Pos) {
            KObj *C = ListEntry(P, KObj, ChildNode);
            StrnCpy(Name, C->Name, KOBJ_NAME_MAX - 1);
            Name[KOBJ_NAME_MAX - 1] = '\0';
            *NameLen = (UINT32)StrLen(Name);
            *Type = C->Type;
            (*Pos)++;
            Rc = 0;
            break;
        }
        Idx++;
    }

    SpinLockReleaseIrq(&GObjLock, F);
    return Rc;
}

/* ============================================================================
 * HANDLE TABLE (per-task)
 * ============================================================================ */

KObj *KObjHandleGet(UINT32 Handle, UINT32 RequiredAccess) {
    Task *Cur = GetCurrentTask();
    if (!Cur || Handle >= KOBJ_HANDLE_MAX) return NULLPTR;

    KObjHandle *H = &Cur->KObjHandles[Handle];
    if (!H->Valid || !H->Object) return NULLPTR;
    if ((H->Access & RequiredAccess) != RequiredAccess) return NULLPTR;

    return H->Object;
}

INT KObjHandleInstall(KObj *Obj, UINT32 Access) {
    Task *Cur = GetCurrentTask();
    if (!Cur || !Obj) return -1;

    for (UINT32 I = 0; I < KOBJ_HANDLE_MAX; I++) {
        if (!Cur->KObjHandles[I].Valid) {
            Cur->KObjHandles[I].Valid  = TRUE;
            Cur->KObjHandles[I].Object = Obj;   /* caller's ref is transferred */
            Cur->KObjHandles[I].Access = Access;
            return (INT)I;
        }
    }
    return -1;  /* table full — caller must KObjUnref */
}

INT KObjHandleClose(UINT32 Handle) {
    Task *Cur = GetCurrentTask();
    if (!Cur || Handle >= KOBJ_HANDLE_MAX) return -1;

    KObjHandle *H = &Cur->KObjHandles[Handle];
    if (!H->Valid) return -1;

    KObj *Obj = H->Object;
    H->Valid  = FALSE;
    H->Object = NULLPTR;
    H->Access = 0;

    if (Obj) {
        if (Obj->Ops && Obj->Ops->Close) Obj->Ops->Close(Obj);
        KObjUnref(Obj);
    }
    return 0;
}