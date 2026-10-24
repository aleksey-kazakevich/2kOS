#pragma once

#include <Drivers/Com.h>
#include <KObj/KObj.h>
#include <Types.h>

VOID KObjComRegisterPort(ComDevice *Dev, UINT8 Index);   /* Index = 0..3 */
KObj *KObjComGet(UINT8 Index);                            /* refcount+1, или NULL */