#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <KObj/Null.h>
#include <Basecon.h>
#include <Return.h>

STATIC KObj *GNullObj = NULLPTR;

STATIC INT NullReadOp(KObj *Obj, UINT64 Off, VOID *Buf, UINT32 Size, UINT32 *Read) {
    (VOID)Obj; (VOID)Off; (VOID)Buf; (VOID)Size;
    *Read = 0;
    return SUCCESS;
}

STATIC INT NullWriteOp(KObj *Obj, UINT64 Off, const VOID *Buf, UINT32 Size, UINT32 *Written) {
    (VOID)Obj; (VOID)Off; (VOID)Buf;
    *Written = Size;
    return SUCCESS;
}

STATIC VOID NullDestroyOp(KObj *Obj) {
    (VOID)Obj;
    GNullObj = NULLPTR;
}

STATIC KObjOperations GNullOps = {
    .Read    = NullReadOp,
    .Write   = NullWriteOp,
    .Destroy = NullDestroyOp,
};

STATIC KObj *EnsureDeviceDir(VOID) {
    KObj *Dev = KObjFindByPath("\\Device");
    if (!Dev) {
        Dev = KObjCreate(KObjGetRoot(), "Device",
                         KOBJ_TYPE_DIRECTORY, NULLPTR, NULLPTR);
    }
    return Dev;
}

VOID KObjInitNull(VOID) {
    if (GNullObj) return;

    KObj *Dev = EnsureDeviceDir();
    if (!Dev) {
        BaseconPrintf(BASECON_TYPE_ERROR, "null: cannot create \\Device\n");
        return;
    }

    GNullObj = KObjCreate(Dev, "Null", KOBJ_TYPE_UNKNOWN, &GNullOps, NULLPTR);
    KObjUnref(Dev);

    if (!GNullObj) {
        BaseconPrintf(BASECON_TYPE_ERROR, "null: cannot create \\Device\\Null\n");
        return;
    }
    GNullObj->Access = KOBJ_ACCESS_BOTH;
}

KObj *KObjGetNull(VOID) {
    if (!GNullObj) return NULLPTR;
    KObjRef(GNullObj);
    return GNullObj;
}