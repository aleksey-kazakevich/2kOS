#pragma once

#include <Types.h>
#include <List.h>

// Driver critical levels
#define DCL0 0 // Driver Critical Level 0 - Critical
#define DCL1 1 // Driver Critical Level 1 - Ordinary
#define DCL2 2 // Driver Critical Level 2 - Additional

struct KDriver;

typedef VOID (*KDriverCallback)(struct KDriver *Self);

typedef struct KDriver {
    ListHead Node;
    CHAR Name[32];
    UINT8 DCL;
    BOOL Initialized;
    VOID *Priv;
    KDriverCallback Shutdown;
} KDriver;

VOID KDriverInit(VOID);
INT KDriverRegister(KDriver *Driver);
INT KDriverUnregister(KDriver *Driver);
KDriver* KDriverGenerateStruct(const CHAR *Name, UINT8 DCL, BOOL Initialized, VOID *Priv, KDriverCallback ShutdownCallback);
KDriver* KDriverFindByName(const CHAR *Name);
UINT32 KDriverGetCount(VOID);
VOID* KDriverGetPrivate(KDriver *Driver, USIZE Size);
KDriver* KDriverGetByIndex(UINT32 Index);

/* If you're want to register driver by one line, use:
 *   KDriverRegister(KDriverGenerateStruct([YourName], [DCL], [Initialized], [Priv]));
 * Where replace [YourName] to your driver name, [DCL] to your DCL (UINT8) (0-2), [Initialized] to TRUE/FALSE, [Priv] to private data or NULLPTR
 */