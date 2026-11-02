#pragma once

#include <KObj/KObj.h>
#include <Types.h>

#define STDIO_RING_SIZE 8192

/* Creates \Kernel\Stdio (and \Kernel if needed). Idempotent. */
VOID KObjInitStdio(VOID);

/* Returns the KObj* of \Kernel\Stdio (refcount is incremented — caller must Unref). */
KObj *KObjGetStdio(VOID);