#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <KObj/Zero.h>
#include <Basecon.h>
#include <Return.h>

STATIC KObj *GZeroObj = NULLPTR;

STATIC INT ZeroReadOp(KObj *Obj, UINT64 Off, VOID *Buf, UINT32 Size, UINT32 *Read) {
    (VOID)Obj; (VOID)Off;
    if (!Buf) return INCORRECT_VALUE;
    MemSet(Buf, 0, Size);
    *Read = Size;
    return SUCCESS;
}

STATIC INT ZeroWriteOp(KObj *Obj, UINT64 Off, const VOID *Buf, UINT32 Size, UINT32 *Written) {
    (VOID)Obj; (VOID)Off; (VOID)Buf;
    *Written = Size;
    return SUCCESS;
}

STATIC VOID ZeroDestroyOp(KObj *Obj) {
    (VOID)Obj;
    GZeroObj = NULLPTR;
}

STATIC KObjOperations GZeroOps = {
    .Read    = ZeroReadOp,
    .Write   = ZeroWriteOp,
    .Destroy = ZeroDestroyOp,
};

STATIC KObj *EnsureDeviceDir(VOID) {
    KObj *Dev = KObjFindByPath("\\Device");
    if (!Dev) {
        Dev = KObjCreate(KObjGetRoot(), "Device",
                         KOBJ_TYPE_DIRECTORY, NULLPTR, NULLPTR);
    }
    return Dev;
}

VOID KObjInitZero(VOID) {
    if (GZeroObj) return;

    KObj *Dev = EnsureDeviceDir();
    if (!Dev) {
        BaseconPrintf(BASECON_TYPE_ERROR, "zero: cannot create \\Device\n");
        return;
    }

    GZeroObj = KObjCreate(Dev, "Zero", KOBJ_TYPE_UNKNOWN, &GZeroOps, NULLPTR);
    KObjUnref(Dev);

    if (!GZeroObj) {
        BaseconPrintf(BASECON_TYPE_ERROR, "zero: cannot create \\Device\\Zero\n");
        return;
    }
    GZeroObj->Access = KOBJ_ACCESS_BOTH;
}

KObj *KObjGetZero(VOID) {
    if (!GZeroObj) return NULLPTR;
    KObjRef(GZeroObj);
    return GZeroObj;
}