#include <Lib/String.h>
#include <SpinLock.h>
#include <KDriver.h>
#include <Return.h>

STATIC ListHead GKDriverList;
STATIC UINT32 GKDriverCount = 0;

STATIC UINT8 GKDriverPrivatePool[8192];
STATIC UINT32 GKDriverPrivateOffset = 0;

STATIC SpinLock GKDriverLock;

VOID KDriverInit(VOID) {
    SpinLockInit(&GKDriverLock);
    ListInit(&GKDriverList);
    GKDriverCount = 0;
}

INT KDriverRegister(KDriver *Driver) {
    if (Driver == NULLPTR) RETURN(NO_OBJECT);

    UINTPTR Flags = SpinLockAcquireIrq(&GKDriverLock);

    // Checking the duplicate under the lock
    ListHead *Pos;
    ListForEach(Pos, &GKDriverList) {
        KDriver *D = ListEntry(Pos, KDriver, Node);
        if (StrCmp(D->Name, Driver->Name) == 0) {
            SpinLockReleaseIrq(&GKDriverLock, Flags);
            RETURN(ALREADY_EXISTS);
        }
    }

    ListAddTail(&GKDriverList, &Driver->Node);
    GKDriverCount++;
    Driver->Initialized = FALSE;

    SpinLockReleaseIrq(&GKDriverLock, Flags);
    RETURN(SUCCESS);
}

INT KDriverUnregister(KDriver *Driver) {
    if (Driver == NULLPTR) RETURN(NO_OBJECT);

    KDriverCallback ShutdownFn = NULLPTR;

    UINTPTR Flags = SpinLockAcquireIrq(&GKDriverLock);
    ShutdownFn = Driver->Shutdown;
    SpinLockReleaseIrq(&GKDriverLock, Flags);

    // Shutdown outside the lock can be long
    if (ShutdownFn) {
        ShutdownFn(Driver);
    }

    Flags = SpinLockAcquireIrq(&GKDriverLock);
    ListDel(&Driver->Node);
    if (GKDriverCount > 0) GKDriverCount--;
    SpinLockReleaseIrq(&GKDriverLock, Flags);

    RETURN(SUCCESS);
}

KDriver* KDriverGenerateStruct(const CHAR *Name, UINT8 DCL, BOOL Initialized,
                                VOID *Priv, KDriverCallback ShutdownCallback) {
    if (!Name) return NULLPTR;

    UINT8 StaticDCL = DCL;
    if (DCL > 2) StaticDCL = 1;

    UINTPTR Flags = SpinLockAcquireIrq(&GKDriverLock);

    if (GKDriverPrivateOffset + sizeof(KDriver) > sizeof(GKDriverPrivatePool)) {
        SpinLockReleaseIrq(&GKDriverLock, Flags);
        return NULLPTR;   // Pool is full
    }

    KDriver *StaticDriver = (KDriver*)&GKDriverPrivatePool[GKDriverPrivateOffset];
    GKDriverPrivateOffset += sizeof(KDriver);

    StrnCpy(StaticDriver->Name, Name, sizeof(StaticDriver->Name) - 1);
    StaticDriver->Name[sizeof(StaticDriver->Name) - 1] = '\0';
    StaticDriver->DCL = StaticDCL;
    StaticDriver->Initialized = Initialized;
    StaticDriver->Priv = Priv;
    StaticDriver->Shutdown = ShutdownCallback;

    SpinLockReleaseIrq(&GKDriverLock, Flags);
    return StaticDriver;
}

KDriver* KDriverFindByName(const CHAR *Name) {
    if (!Name) return NULLPTR;

    UINTPTR Flags = SpinLockAcquireIrq(&GKDriverLock);

    KDriver *Result = NULLPTR;
    ListHead *Pos;
    ListForEach(Pos, &GKDriverList) {
        KDriver *Driver = ListEntry(Pos, KDriver, Node);
        if (StrCmp(Driver->Name, Name) == 0) {
            Result = Driver;
            break;
        }
    }

    SpinLockReleaseIrq(&GKDriverLock, Flags);
    return Result;
}

UINT32 KDriverGetCount(VOID) {
    return GKDriverCount;
}

VOID* KDriverGetPrivate(KDriver *Driver, USIZE Size) {
    if (Driver == NULLPTR) return NULLPTR;

    UINTPTR Flags = SpinLockAcquireIrq(&GKDriverLock);

    // If the private account has already been allocated, we return it.
    if (Driver->Priv != NULLPTR) {
        SpinLockReleaseIrq(&GKDriverLock, Flags);
        return Driver->Priv;
    }

    // Checking the place in the pool
    if (GKDriverPrivateOffset + Size > sizeof(GKDriverPrivatePool)) {
        SpinLockReleaseIrq(&GKDriverLock, Flags);
        return NULLPTR;
    }

    VOID *Priv = (VOID*)&GKDriverPrivatePool[GKDriverPrivateOffset];
    GKDriverPrivateOffset += Size;

    Driver->Priv = Priv;
    MemSet(Priv, 0, Size);

    SpinLockReleaseIrq(&GKDriverLock, Flags);
    return Priv;
}

KDriver* KDriverGetByIndex(UINT32 Index) {
    UINTPTR Flags = SpinLockAcquireIrq(&GKDriverLock);

    KDriver *Result = NULLPTR;
    ListHead *Pos;
    UINT32 Current = 0;

    ListForEach(Pos, &GKDriverList) {
        if (Current == Index) {
            Result = ListEntry(Pos, KDriver, Node);
            break;
        }
        Current++;
    }

    SpinLockReleaseIrq(&GKDriverLock, Flags);
    return Result;
}