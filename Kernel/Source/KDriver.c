#include <KDriver.h>
#include <Return.h>
#include <Lib/String.h>

static ListHead GKDriverList;
static UINT32 GKDriverCount = 0;

static UINT8 GKDriverPrivatePool[8192];
static UINT32 GKDriverPrivateOffset = 0;

VOID KDriverInit(VOID) {
    ListInit(&GKDriverList);
    GKDriverCount = 0;
}

INT KDriverRegister(KDriver *Driver) {
    if (Driver == NULLPTR) RETURN(NO_OBJECT);
    if (KDriverFindByName(Driver->Name)) {
	RETURN(ALREADY_EXISTS);
    }
    ListAddTail(&GKDriverList, &Driver->Node);
    GKDriverCount++;
    Driver->Initialized = FALSE;
    RETURN(SUCCESS);
}

INT KDriverUnregister(KDriver *Driver) {
    if (Driver == NULLPTR) RETURN(NO_OBJECT);
    if (Driver->Shutdown != NULLPTR) {
        Driver->Shutdown(Driver);
    }
    ListDel(&Driver->Node);
    GKDriverCount--;
    RETURN(SUCCESS);
}
KDriver* KDriverGenerateStruct(const CHAR *Name, UINT8 DCL, BOOL Initialized, VOID *Priv, KDriverCallback ShutdownCallback) {
    UINT8 StaticDCL = DCL;
    if (DCL > 2) StaticDCL = 1;
    
    // Allocating memory from the pool or using MemoryAllocate
    KDriver *StaticDriver = (KDriver*)&GKDriverPrivatePool[GKDriverPrivateOffset];
    if (GKDriverPrivateOffset + sizeof(KDriver) > 8192) {
        return NULLPTR;  // Pool is full
    }
    GKDriverPrivateOffset += sizeof(KDriver);
    
    StrCpy(StaticDriver->Name, Name);
    StaticDriver->DCL = StaticDCL;
    StaticDriver->Initialized = Initialized;
    StaticDriver->Priv = Priv;
    StaticDriver->Shutdown = ShutdownCallback;
    
    return StaticDriver;
}

KDriver* KDriverFindByName(const CHAR *Name) {
    ListHead *Pos;
    ListForEach(Pos, &GKDriverList) {
        KDriver *Driver = ListEntry(Pos, KDriver, Node);
	if (StrCmp(Driver->Name, Name) == 0) {
	    return Driver;
	}
    }
    return NULLPTR;
}

UINT32 KDriverGetCount(VOID) {
    return GKDriverCount;
}

VOID* KDriverGetPrivate(KDriver *Driver, USIZE Size) {
    if (Driver == NULLPTR) return NULLPTR;
    
    // If the data is already allocated, simply return it
    if (Driver->Priv != NULLPTR) {
        return Driver->Priv;
    }
    
    // Checking if there is enough space in the pool
    if (GKDriverPrivateOffset + Size > 8192) {
        return NULLPTR; 
    }
    
    // Select from the pool
    Driver->Priv = (VOID*)&GKDriverPrivatePool[GKDriverPrivateOffset];
    GKDriverPrivateOffset += Size;
    
    // Resetting memory
    MemSet(Driver->Priv, 0, Size);
    
    return Driver->Priv;
}

KDriver* KDriverGetByIndex(UINT32 Index) {
    ListHead *Pos;
    UINT32 Current = 0;
    
    ListForEach(Pos, &GKDriverList) {
        if (Current == Index) {
            return ListEntry(Pos, KDriver, Node);
        }
        Current++;
    }
    return NULLPTR;
}